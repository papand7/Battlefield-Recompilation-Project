// navgen: a map's navigation mesh for the bots (Battlefield 1943 and Bad
// Company), made with Recast from its collision (OBJ files: the runtime's
// BF_BOT_DUMP_COLLISION and tools/terrain.py's ground), saved as Detour data.
// Private: the collision comes from the player's own copy; only the .nav it
// makes ships.
//
//   navgen <out.nav> <in.obj>... [--bounds x0 z0 x1 z1] [--cell 0.3] [--obj walkable.obj]
//          [--radius 0.7] [--climb 0.4]
//
// A soldier: radius 0.7 m (3 cells: more than its body, so routes keep clear of wall
// corners), height 1.8 m, step 0.4 m, slopes up to 45 degrees.
// Recast's single-mesh pipeline (as its Sample_SoloMesh): voxelise the
// walkable triangles, filter ledges and low ceilings, erode by the radius,
// regions (watershed), contours, polygons, the detail mesh, then Detour's
// dtCreateNavMeshData. The file: "BFNV", version 1, the data's size, the data.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"

namespace {

struct Mesh {
  std::vector<float> verts;  // x y z
  std::vector<int> tris;
};

bool LoadObj(const char* path, Mesh& m, const float* bounds) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  int base = int(m.verts.size() / 3);
  std::vector<int> map;  // the file's vertex -> ours (-1 out of bounds)
  std::vector<float> fv;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == 'v' && line[1] == ' ') {
      float x, y, z;
      if (sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3) fv.push_back(x), fv.push_back(y), fv.push_back(z);
    } else if (line[0] == 'f' && line[1] == ' ') {
      int a, b, c;
      if (sscanf(line + 2, "%d %d %d", &a, &b, &c) != 3) {
        // "f a/x/y b/x/y c/x/y"
        if (sscanf(line + 2, "%d/%*s %d/%*s %d/%*s", &a, &b, &c) != 3) continue;
      }
      int idx[3] = {a - 1, b - 1, c - 1};
      bool in = true;
      for (int i : idx) {
        if (i < 0 || i * 3 + 2 >= int(fv.size())) in = false;
        else if (bounds && (fv[i * 3] < bounds[0] || fv[i * 3] > bounds[2] || fv[i * 3 + 2] < bounds[1] ||
                            fv[i * 3 + 2] > bounds[3]))
          in = false;
      }
      if (!in) continue;
      if (map.size() < fv.size() / 3) map.resize(fv.size() / 3, -1);
      for (int i : idx) {
        if (map[i] < 0) {
          map[i] = int(m.verts.size() / 3);
          m.verts.insert(m.verts.end(), fv.begin() + i * 3, fv.begin() + i * 3 + 3);
        }
        m.tris.push_back(map[i]);
      }
    }
  }
  fclose(f);
  (void)base;
  return true;
}

void WriteWalkableObj(const char* path, const rcPolyMesh& pm) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  for (int i = 0; i < pm.nverts; ++i) {
    const unsigned short* v = &pm.verts[i * 3];
    fprintf(f, "v %.2f %.2f %.2f\n", pm.bmin[0] + v[0] * pm.cs, pm.bmin[1] + (v[1] + 1) * pm.ch, pm.bmin[2] + v[2] * pm.cs);
  }
  for (int i = 0; i < pm.npolys; ++i) {
    const unsigned short* p = &pm.polys[i * pm.nvp * 2];
    int n = 0;
    while (n < pm.nvp && p[n] != RC_MESH_NULL_IDX) ++n;
    for (int k = 1; k + 1 < n; ++k) fprintf(f, "f %d %d %d\n", p[0] + 1, p[k] + 1, p[k + 1] + 1);
  }
  fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "navgen <out.nav> <in.obj>... [--bounds x0 z0 x1 z1] [--cell 0.3] [--obj walkable.obj]\n");
    return 2;
  }
  const char* out = argv[1];
  std::vector<const char*> inputs;
  float bounds[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
  bool have_bounds = false;
  float cell = 0.3f, radius = 0.7f, climb = 0.4f;  // in cells: 3 (0.9 m) and 2 (0.4 m)
  const char* walkable_obj = nullptr;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--bounds") && i + 4 < argc) {
      for (int k = 0; k < 4; ++k) bounds[k] = float(atof(argv[++i]));
      have_bounds = true;
    } else if (!strcmp(argv[i], "--cell") && i + 1 < argc) {
      cell = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--radius") && i + 1 < argc) {
      radius = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--climb") && i + 1 < argc) {
      climb = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--obj") && i + 1 < argc) {
      walkable_obj = argv[++i];
    } else {
      inputs.push_back(argv[i]);
    }
  }
  Mesh mesh;
  for (const char* in : inputs)
    if (!LoadObj(in, mesh, have_bounds ? bounds : nullptr)) fprintf(stderr, "navgen: cannot read %s\n", in);
  int nverts = int(mesh.verts.size() / 3), ntris = int(mesh.tris.size() / 3);
  printf("navgen: %d triangles, %d vertices\n", ntris, nverts);
  if (!ntris) return 1;

  // A soldier.
  const float agent_height = 1.8f, agent_radius = radius, agent_climb = climb, slope = 45.0f;
  rcConfig cfg{};
  cfg.cs = cell;
  cfg.ch = 0.2f;
  cfg.walkableSlopeAngle = slope;
  cfg.walkableHeight = int(std::ceil(agent_height / cfg.ch));
  cfg.walkableClimb = int(std::floor(agent_climb / cfg.ch));
  cfg.walkableRadius = int(std::ceil(agent_radius / cfg.cs));
  cfg.maxEdgeLen = int(12.0f / cfg.cs);
  cfg.maxSimplificationError = 1.3f;
  cfg.minRegionArea = 8 * 8;
  cfg.mergeRegionArea = 20 * 20;
  cfg.maxVertsPerPoly = 6;
  cfg.detailSampleDist = cfg.cs * 6;
  cfg.detailSampleMaxError = cfg.ch;
  rcCalcBounds(mesh.verts.data(), nverts, cfg.bmin, cfg.bmax);
  rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
  printf("navgen: grid %d x %d over %.0f..%.0f, %.0f..%.0f\n", cfg.width, cfg.height, cfg.bmin[0], cfg.bmax[0],
         cfg.bmin[2], cfg.bmax[2]);

  rcContext ctx;
  rcHeightfield* hf = rcAllocHeightfield();
  if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return 1;
  std::vector<unsigned char> areas(ntris, 0);
  rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, mesh.verts.data(), nverts, mesh.tris.data(), ntris, areas.data());
  if (!rcRasterizeTriangles(&ctx, mesh.verts.data(), nverts, mesh.tris.data(), areas.data(), ntris, *hf, cfg.walkableClimb))
    return 1;
  rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
  rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
  rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

  rcCompactHeightfield* chf = rcAllocCompactHeightfield();
  if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf)) return 1;
  rcFreeHeightField(hf);
  if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) return 1;
  if (!rcBuildDistanceField(&ctx, *chf)) return 1;
  if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) return 1;
  rcContourSet* cset = rcAllocContourSet();
  if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) return 1;
  rcPolyMesh* pmesh = rcAllocPolyMesh();
  if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) return 1;
  rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
  if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) return 1;
  rcFreeCompactHeightfield(chf);
  rcFreeContourSet(cset);
  printf("navgen: %d polygons, %d vertices\n", pmesh->npolys, pmesh->nverts);
  if (walkable_obj) WriteWalkableObj(walkable_obj, *pmesh);

  // Every polygon walkable (area 0 from Recast's walkable marking -> flag 1).
  for (int i = 0; i < pmesh->npolys; ++i) {
    if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->areas[i] = 0;
    pmesh->flags[i] = 1;
  }
  if (pmesh->nverts >= 0xFFFF) {
    fprintf(stderr, "navgen: too many vertices for one Detour tile (%d); use a bigger --cell\n", pmesh->nverts);
    return 1;
  }
  dtNavMeshCreateParams p{};
  p.verts = pmesh->verts;
  p.vertCount = pmesh->nverts;
  p.polys = pmesh->polys;
  p.polyAreas = pmesh->areas;
  p.polyFlags = pmesh->flags;
  p.polyCount = pmesh->npolys;
  p.nvp = pmesh->nvp;
  p.detailMeshes = dmesh->meshes;
  p.detailVerts = dmesh->verts;
  p.detailVertsCount = dmesh->nverts;
  p.detailTris = dmesh->tris;
  p.detailTriCount = dmesh->ntris;
  p.walkableHeight = agent_height;
  p.walkableRadius = agent_radius;
  p.walkableClimb = agent_climb;
  rcVcopy(p.bmin, pmesh->bmin);
  rcVcopy(p.bmax, pmesh->bmax);
  p.cs = cfg.cs;
  p.ch = cfg.ch;
  p.buildBvTree = true;
  unsigned char* data = nullptr;
  int size = 0;
  if (!dtCreateNavMeshData(&p, &data, &size)) {
    fprintf(stderr, "navgen: Detour could not make the navigation data\n");
    return 1;
  }
  FILE* f = fopen(out, "wb");
  if (!f) return 1;
  const char magic[4] = {'B', 'F', 'N', 'V'};
  int version = 1;
  fwrite(magic, 1, 4, f);
  fwrite(&version, 4, 1, f);
  fwrite(&size, 4, 1, f);
  fwrite(data, 1, size_t(size), f);
  fclose(f);
  dtFree(data);
  printf("navgen: %d bytes of navigation data to %s\n", size, out);
  return 0;
}
