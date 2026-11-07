// Loads a XEX through XenonUtils (decrypt + decompress) and writes the flat
// memory image (base..base+size) so other tools can scan it.
// Usage: xex_dump <in.xex> <out.bin> [symbols.txt]
// Output is game-derived: keep it inside bf1943-recomp/generated only.
#include <cstdio>
#include <cstring>
#include <vector>
#include <image.h>
#include <file.h>

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: xex_dump <in.xex> <out.bin> [symbols.txt]\n"); return 1; }
    auto file = LoadFile(argv[1]);
    if (file.empty()) { printf("cannot read %s\n", argv[1]); return 1; }
    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data) { printf("ParseImage failed\n"); return 2; }
    printf("base=0x%zX size=0x%X entry=0x%zX\n", image.base, image.size, image.entry_point);
    const uint8_t* d = image.data.get();
    printf("first bytes: %02X %02X (MZ expected)\n", d[0], d[1]);
    for (auto& s : image.sections)
        printf("section %-8s 0x%08zX size 0x%08X flags %u\n", s.name.c_str(), s.base, s.size, (unsigned)s.flags);
    size_t nsym = 0;
    for (auto& s : image.symbols) { (void)s; nsym++; }
    printf("symbols (imports/thunks etc.): %zu\n", nsym);
    FILE* f = fopen(argv[2], "wb");
    if (!f) return 3;
    fwrite(d, 1, image.size, f);
    fclose(f);
    if (argc > 3)
    {
        FILE* g = fopen(argv[3], "w");
        for (auto& s : image.symbols)
            fprintf(g, "0x%08zX 0x%zX %d %s\n", s.address, s.size, (int)s.type, s.name.c_str());
        fclose(g);
    }
    return 0;
}
