/* Android: no Media Foundation and no FFmpeg (this game has no movies): nn::movie keeps nnmovie.c's failing placeholders
 * and the album records no movies (nnalbum.c asks movie_mf_present first). Linux and Windows build nnmovie_mf.c instead. */
int movie_mf_present(void) { return 0; }
void register_movie_mf_imports(void) {}
