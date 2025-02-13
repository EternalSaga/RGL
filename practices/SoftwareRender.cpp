#include "SoftwareRender.hpp"
namespace RGL
{
namespace swr
{

void
set_pixel(SDL_Surface *surface, int x, int y, Uint32 pixel)
{
    const SDL_PixelFormatDetails* format_details = SDL_GetPixelFormatDetails(surface->format);
    Uint32 *target_pixel =  (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch + x * format_details->bytes_per_pixel); // 使用 format_details->BytesPerPixel
    *target_pixel = pixel;
}
TestCPURender::TestCPURender(SDL_Surface *surface) : surface(surface)
{
    width = surface->w;
    height = surface->h;
}
void
TestCPURender::operator()()
{
    auto red = SDL_MapSurfaceRGBA(surface, 255, 0, 0, 0);
    SDL_LockSurface(surface);
    for (size_t i = 0; i < width; i++) {
	for (size_t j = 0; j < height; j++) {
	    set_pixel(surface, i, j, red);
	}
    }
    SDL_UnlockSurface(surface);
}
} // namespace swr
} // namespace RGL