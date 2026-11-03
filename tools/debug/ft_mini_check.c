/* ft_mini_check <font>...: every glyph of each font through runtime/plat/ft_mini.h (the Linux / Android outline reader),
 * the failures counted. The fonts: SSBU_FONTDUMP=<folder> on any run writes the game's decoded fonts there.
 *   clang -O2 -o ft_mini_check tools/debug/ft_mini_check.c && ./ft_mini_check <the .otf and .ttf files of that folder>
 * 2026-11-03, the game's 12 fonts (7 CFF, CID-keyed): 81,000 glyphs, 0 failed. */
#include <stdio.h>
#include <stdlib.h>
#include "../../runtime/plat/ft_mini.h"

int main(int argc, char **argv) {
    int failed = 0;
    for (int a = 1; a < argc; a++) {
        FT_Library lib;
        FT_Face f;
        FILE *in = fopen(argv[a], "rb");
        if (!in) { printf("%s: cannot be read\n", argv[a]); failed++; continue; }
        fseek(in, 0, SEEK_END);
        long n = ftell(in);
        fseek(in, 0, SEEK_SET);
        unsigned char *d = malloc((size_t)n);
        if (!d || fread(d, 1, (size_t)n, in) != (size_t)n) { fclose(in); free(d); failed++; continue; }
        fclose(in);
        FT_Init_FreeType(&lib);
        if (FT_New_Memory_Face(lib, d, n, 0, &f)) { printf("%-40s refused\n", argv[a]); failed++; free(d); continue; }
        int bad = 0, empty = 0;
        for (int g = 0; g < f->num_glyphs; g++) {
            if (FT_Load_Glyph(f, (unsigned)g, FT_LOAD_NO_SCALE)) { if (bad++ < 3) printf("   glyph %d failed\n", g); continue; }
            empty += !f->glyph->outline.n_points;
        }
        printf("%-40s %6d glyphs (%s): %d failed, %d empty\n", argv[a], f->num_glyphs, f->cff ? "CFF" : "TrueType", bad, empty);
        failed += bad;
    }
    return failed != 0;
}
