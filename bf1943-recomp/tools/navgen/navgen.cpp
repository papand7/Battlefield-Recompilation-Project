// navgen: a map's navigation mesh for the bots (Battlefield 1943 and Bad
// Company), made with Recast from its collision (OBJ files: the runtime's
// BF_BOT_DUMP_COLLISION and tools/terrain.py's ground), saved as Detour data.
// Private: the collision comes from the player's own copy; only the .nav it
// makes ships.
//
//   navgen <out.nav> <in.obj>... [--bounds x0 z0 x1 z1] [--cell 0.3] [--obj walkable.obj]
//          [--radius 0.7] [--climb 0.4] [--slope 45]   (a vehicle's: --cell 0.4 --radius 2
//          --climb 0.6 --slope 35)
//   navgen --islands <in.nav> [x y z]...   which joined piece of the mesh each
//          point is on (and the biggest pieces), to see why a route can't be found
//
// A soldier: radius 0.7 m (3 cells: more than its body, so routes keep clear of wall
// corners), height 1.8 m, step 0.4 m, slopes up to 45 degrees.
// Recast's single-mesh pipeline (as its Sample_SoloMesh): voxelise the
// walkable triangles, filter ledges and low ceilings, erode by the radius,
// regions (watershed), contours, polygons, the detail mesh, then Detour's
// dtCreateNavMeshData, in tiles of 256 cells. The file: see main (version 2).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
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

}  // namespace

// --islands: the mesh's joined pieces (polygons reached through their links),
// each point's piece, and the 12 biggest with their extent.
// A .nav file: version 1 one tile, version 2 tiles (see main).
bool LoadNav(const char* path, dtNavMesh& mesh) {
  FILE* f = fopen(path, "rb");
  if (!f) return fprintf(stderr, "can't open %s\n", path), false;
  char magic[4];
  int version = 0;
  bool ok = fread(magic, 1, 4, f) == 4 && fread(&version, 4, 1, f) == 1;
  if (ok && version == 1) {
    int size = 0;
    ok = fread(&size, 4, 1, f) == 1;
    unsigned char* data = ok ? static_cast<unsigned char*>(dtAlloc(size_t(size), DT_ALLOC_PERM)) : nullptr;
    ok = data && fread(data, 1, size_t(size), f) == size_t(size) && dtStatusSucceed(mesh.init(data, size, DT_TILE_FREE_DATA));
  } else if (ok && version == 2) {
    dtNavMeshParams params;
    int count = 0;
    ok = fread(&params, sizeof(params), 1, f) == 1 && fread(&count, 4, 1, f) == 1 && dtStatusSucceed(mesh.init(&params));
    for (int i = 0; ok && i < count; ++i) {
      int tx, ty, size;
      ok = fread(&tx, 4, 1, f) == 1 && fread(&ty, 4, 1, f) == 1 && fread(&size, 4, 1, f) == 1;
      unsigned char* data = ok ? static_cast<unsigned char*>(dtAlloc(size_t(size), DT_ALLOC_PERM)) : nullptr;
      ok = data && fread(data, 1, size_t(size), f) == size_t(size) &&
           dtStatusSucceed(mesh.addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr));
    }
  } else {
    ok = false;
  }
  fclose(f);
  if (!ok) fprintf(stderr, "bad mesh %s\n", path);
  return ok;
}

// --islands: the mesh's joined pieces (polygons reached through their links,
// across tiles), each point's piece, and the 12 biggest with their extent.
int Islands(int argc, char** argv) {
  dtNavMesh mesh;
  if (!LoadNav(argv[2], mesh)) return 1;
  dtNavMeshQuery q;
  q.init(&mesh, 2048);
  const dtNavMesh& cm = mesh;
  std::unordered_map<dtPolyRef, int> piece;
  struct Info { int polys = 0; float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}; };
  std::vector<Info> info;
  int n = 0;
  for (int ti = 0; ti < cm.getMaxTiles(); ++ti) {
    const dtMeshTile* tile = cm.getTile(ti);
    if (!tile || !tile->header) continue;
    dtPolyRef base = cm.getPolyRefBase(tile);
    for (int pi = 0; pi < tile->header->polyCount; ++pi) {
      ++n;
      dtPolyRef start = base | dtPolyRef(pi);
      if (piece.count(start)) continue;
      int id = int(info.size());
      info.push_back({});
      std::vector<dtPolyRef> stack{start};
      piece[start] = id;
      while (!stack.empty()) {
        dtPolyRef r = stack.back();
        stack.pop_back();
        const dtMeshTile* t = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusFailed(cm.getTileAndPolyByRef(r, &t, &poly))) continue;
        Info& in = info[size_t(id)];
        ++in.polys;
        for (int v = 0; v < poly->vertCount; ++v)
          for (int k = 0; k < 3; ++k) {
            float c = t->verts[poly->verts[v] * 3 + k];
            in.lo[k] = std::min(in.lo[k], c), in.hi[k] = std::max(in.hi[k], c);
          }
        for (unsigned l = poly->firstLink; l != DT_NULL_LINK; l = t->links[l].next) {
          dtPolyRef o = t->links[l].ref;
          if (o && !piece.count(o)) piece[o] = id, stack.push_back(o);
        }
      }
    }
  }
  std::vector<int> order(info.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return info[size_t(a)].polys > info[size_t(b)].polys; });
  printf("%d polygons in %zu pieces; the biggest:\n", n, info.size());
  for (size_t i = 0; i < order.size() && i < 12; ++i) {
    const Info& in = info[size_t(order[i])];
    printf("  piece %d: %d polygons, x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f\n", order[i], in.polys, in.lo[0], in.hi[0],
           in.lo[1], in.hi[1], in.lo[2], in.hi[2]);
  }
  dtQueryFilter filter;
  for (int i = 3; i + 2 < argc; i += 3) {
    float p[3] = {float(atof(argv[i])), float(atof(argv[i + 1])), float(atof(argv[i + 2]))}, near[3];
    const float ext[3] = {3, 6, 3};
    dtPolyRef r = 0;
    q.findNearestPoly(p, ext, &filter, &r, near);
    if (!r) {
      printf("%.1f %.1f %.1f: no polygon near\n", p[0], p[1], p[2]);
      continue;
    }
    int id = piece.count(r) ? piece[r] : -1;
    printf("%.1f %.1f %.1f: piece %d (%d polygons), nearest %.1f %.1f %.1f\n", p[0], p[1], p[2], id,
           id >= 0 ? info[size_t(id)].polys : 0, near[0], near[1], near[2]);
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && !strcmp(argv[1], "--islands")) return Islands(argc, argv);
  if (argc < 3) {
    fprintf(stderr, "navgen <out.nav> <in.obj>... [--bounds x0 z0 x1 z1] [--cell 0.3] [--obj walkable.obj]\n");
    return 2;
  }
  const char* out = argv[1];
  std::vector<const char*> inputs;
  float bounds[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
  bool have_bounds = false;
  float cell = 0.3f, radius = 0.7f, climb = 0.4f;  // in cells: 3 (0.9 m) and 2 (0.4 m)
  float slope_limit = 45.0f;
  const char* walkable_obj = nullptr;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--bounds") && i + 4 < argc) {
      for (int k = 0; k < 4; ++k) bounds[k] = float(atof(argv[++i]));
      have_bounds = true;
    } else if (!strcmp(argv[i], "--cell") && i + 1 < argc) {
      cell = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--radius") && i + 1 < argc) {
      radius = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--slope") && i + 1 < argc) {
      slope_limit = float(atof(argv[++i]));
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
  const float agent_height = 1.8f, agent_radius = radius, agent_climb = climb, slope = slope_limit;
  rcConfig base{};
  base.cs = cell;
  base.ch = 0.2f;
  base.walkableSlopeAngle = slope;
  base.walkableHeight = int(std::ceil(agent_height / base.ch));
  base.walkableClimb = int(std::floor(agent_climb / base.ch));
  base.walkableRadius = int(std::ceil(agent_radius / base.cs));
  base.maxEdgeLen = int(12.0f / base.cs);
  base.maxSimplificationError = 1.3f;
  base.minRegionArea = 8 * 8;
  base.mergeRegionArea = 20 * 20;
  base.maxVertsPerPoly = 6;
  base.detailSampleDist = base.cs * 6;
  base.detailSampleMaxError = base.ch;
  // Tiles of 256 cells (51.2 m at 0.2 m), each built with a border of the
  // soldier's radius + 3 cells so they join: one tile alone overflowed
  // Detour's 65535 vertices and Recast's region ids on the larger maps.
  base.tileSize = 256;
  base.borderSize = base.walkableRadius + 3;
  float bmin[3], bmax[3];
  rcCalcBounds(mesh.verts.data(), nverts, bmin, bmax);
  int gw = 0, gh = 0;
  rcCalcGridSize(bmin, bmax, base.cs, &gw, &gh);
  const int ts = base.tileSize;
  const int tw = (gw + ts - 1) / ts, th = (gh + ts - 1) / ts;
  const float tcs = ts * base.cs;
  printf("navgen: grid %d x %d over %.0f..%.0f, %.0f..%.0f: %d x %d tiles of %.1f m\n", gw, gh, bmin[0], bmax[0], bmin[2],
         bmax[2], tw, th, tcs);
  // Each triangle listed in every tile its extent (and the border) touches.
  std::vector<std::vector<int>> tile_tris(size_t(tw) * th);
  const float pad = base.borderSize * base.cs;
  for (int t = 0; t < ntris; ++t) {
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (int k = 0; k < 3; ++k) {
      const float* v = &mesh.verts[size_t(mesh.tris[t * 3 + k]) * 3];
      lo[0] = std::min(lo[0], v[0]), hi[0] = std::max(hi[0], v[0]);
      lo[1] = std::min(lo[1], v[2]), hi[1] = std::max(hi[1], v[2]);
    }
    int x0 = std::max(0, int((lo[0] - pad - bmin[0]) / tcs)), x1 = std::min(tw - 1, int((hi[0] + pad - bmin[0]) / tcs));
    int y0 = std::max(0, int((lo[1] - pad - bmin[2]) / tcs)), y1 = std::min(th - 1, int((hi[1] + pad - bmin[2]) / tcs));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) tile_tris[size_t(y) * tw + x].push_back(t);
  }
  struct Tile { int x, y; unsigned char* data; int size; };
  std::vector<Tile> tiles;
  rcContext ctx;
  int total_polys = 0;
  FILE* walk = walkable_obj ? fopen(walkable_obj, "w") : nullptr;
  int walk_verts = 0;
  for (int ty = 0; ty < th; ++ty)
    for (int tx = 0; tx < tw; ++tx) {
      const std::vector<int>& list = tile_tris[size_t(ty) * tw + tx];
      if (list.empty()) continue;
      rcConfig cfg = base;
      cfg.width = cfg.tileSize + cfg.borderSize * 2;
      cfg.height = cfg.tileSize + cfg.borderSize * 2;
      cfg.bmin[0] = bmin[0] + tx * tcs - pad, cfg.bmin[1] = bmin[1], cfg.bmin[2] = bmin[2] + ty * tcs - pad;
      cfg.bmax[0] = bmin[0] + (tx + 1) * tcs + pad, cfg.bmax[1] = bmax[1], cfg.bmax[2] = bmin[2] + (ty + 1) * tcs + pad;
      std::vector<int> tris;
      tris.reserve(list.size() * 3);
      for (int t : list) tris.insert(tris.end(), mesh.tris.begin() + t * 3, mesh.tris.begin() + t * 3 + 3);
      int n = int(list.size());
      rcHeightfield* hf = rcAllocHeightfield();
      if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return 1;
      std::vector<unsigned char> areas(size_t(n), 0);
      rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, mesh.verts.data(), nverts, tris.data(), n, areas.data());
      if (!rcRasterizeTriangles(&ctx, mesh.verts.data(), nverts, tris.data(), areas.data(), n, *hf, cfg.walkableClimb))
        return 1;
      rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
      rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
      rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
      rcCompactHeightfield* chf = rcAllocCompactHeightfield();
      bool ok = rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf);
      rcFreeHeightField(hf);
      ok = ok && rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) && rcBuildDistanceField(&ctx, *chf) &&
           rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea);
      rcContourSet* cset = rcAllocContourSet();
      ok = ok && rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);
      rcPolyMesh* pmesh = rcAllocPolyMesh();
      ok = ok && cset->nconts > 0 && rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh);
      rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
      ok = ok && pmesh->npolys > 0 &&
           rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);
      rcFreeCompactHeightfield(chf);
      rcFreeContourSet(cset);
      if (ok && pmesh->nverts < 0xFFFF) {
        // Every polygon walkable (area 0 from Recast's walkable marking -> flag 1).
        for (int i = 0; i < pmesh->npolys; ++i) {
          if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->areas[i] = 0;
          pmesh->flags[i] = 1;
        }
        if (walk) {
          for (int i = 0; i < pmesh->nverts; ++i) {
            const unsigned short* v = &pmesh->verts[i * 3];
            fprintf(walk, "v %.2f %.2f %.2f\n", pmesh->bmin[0] + v[0] * pmesh->cs, pmesh->bmin[1] + (v[1] + 1) * pmesh->ch,
                    pmesh->bmin[2] + v[2] * pmesh->cs);
          }
          for (int i = 0; i < pmesh->npolys; ++i) {
            const unsigned short* q = &pmesh->polys[i * pmesh->nvp * 2];
            int k = 0;
            while (k < pmesh->nvp && q[k] != RC_MESH_NULL_IDX) ++k;
            for (int j = 1; j + 1 < k; ++j)
              fprintf(walk, "f %d %d %d\n", walk_verts + q[0] + 1, walk_verts + q[j] + 1, walk_verts + q[j + 1] + 1);
          }
          walk_verts += pmesh->nverts;
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
        p.tileX = tx;
        p.tileY = ty;
        p.tileLayer = 0;
        rcVcopy(p.bmin, pmesh->bmin);
        rcVcopy(p.bmax, pmesh->bmax);
        p.cs = cfg.cs;
        p.ch = cfg.ch;
        p.buildBvTree = true;
        unsigned char* data = nullptr;
        int size = 0;
        if (dtCreateNavMeshData(&p, &data, &size)) tiles.push_back({tx, ty, data, size}), total_polys += pmesh->npolys;
      } else if (ok) {
        fprintf(stderr, "navgen: tile %d,%d has too many vertices (%d)\n", tx, ty, pmesh->nverts);
      }
      rcFreePolyMesh(pmesh);
      rcFreePolyMeshDetail(dmesh);
    }
  if (walk) fclose(walk);
  printf("navgen: %d polygons in %zu tiles\n", total_polys, tiles.size());
  if (tiles.empty()) return 1;
  // The file (version 2): "BFNV", 2, the mesh's parameters (dtNavMeshParams:
  // origin, tile width and height, max tiles, max polygons per tile), the tile
  // count, then each tile: x, y, its data's size, the data.
  dtNavMeshParams params{};
  rcVcopy(params.orig, bmin);
  params.tileWidth = tcs;
  params.tileHeight = tcs;
  int tile_bits = 0;
  while ((1 << tile_bits) < tw * th) ++tile_bits;
  tile_bits = std::min(tile_bits, 14);
  params.maxTiles = 1 << tile_bits;
  params.maxPolys = 1 << (22 - tile_bits);
  FILE* f = fopen(out, "wb");
  if (!f) return 1;
  const char magic[4] = {'B', 'F', 'N', 'V'};
  int version = 2, count = int(tiles.size());
  fwrite(magic, 1, 4, f);
  fwrite(&version, 4, 1, f);
  fwrite(&params, sizeof(params), 1, f);
  fwrite(&count, 4, 1, f);
  size_t bytes = 0;
  for (const Tile& t : tiles) {
    fwrite(&t.x, 4, 1, f);
    fwrite(&t.y, 4, 1, f);
    fwrite(&t.size, 4, 1, f);
    fwrite(t.data, 1, size_t(t.size), f);
    bytes += size_t(t.size);
    dtFree(t.data);
  }
  fclose(f);
  printf("navgen: %zu bytes of navigation data (%zu tiles) to %s\n", bytes, tiles.size(), out);
  return 0;
}
