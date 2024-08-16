#include <SDL2/SDL.h>
#include <stdbool.h>

static unsigned int* raster = NULL;
static unsigned int** raster2d = NULL;
static int width = 0;
static int height = 0;
static int s_width = 0;
static int s_height = 0;
static bool quit = false;
static const bool fullscreen = false; // set flag to toggle fullscreen

static SDL_Window* window = NULL;
static SDL_Texture* texture = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Event event;

static void setup_data(void);
static void handle_key_down(SDL_Keysym* keysym);
static void check_sdl_events(SDL_Event event);
static void make_current_context(void);

static void init_data(void) {
	printf("malloc raster w:%d h:%d", s_width, s_height);
	raster = (unsigned int*)malloc((s_width * s_height) * sizeof(unsigned int));
	for (int r = 0; r < s_width * s_height; r++) {
		raster[r] = 0;
	}
	raster2d = (unsigned int**)malloc(s_width * sizeof(unsigned int*));
	if (raster2d == NULL) {
		printf("out of memory");
	}
	for (int i = 0; i < s_width; i++) {
		raster2d[i] = (unsigned int*)malloc(s_height * sizeof(unsigned int));
		if (raster2d[i] == NULL) {
			printf("out of memory");
		}
	}
	for (int x = 0; x < s_width; x++) {
		for (int y = 0; y < s_height; y++) {
			raster2d[x][y] = 0;
		}
	}
}

static void handle_key_down(SDL_Keysym* keysym) {
	switch (keysym->sym) {
	case SDLK_ESCAPE:
		quit = true;
		break;
	default:
		break;
	}
}

static void check_sdl_events(SDL_Event event) {
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			quit = true;
			break;
		case SDL_KEYDOWN:
			handle_key_down(&event.key.keysym);
			break;
		}
	}
}

void sdl2_init_video(void) {
	width = 320 * 2;
	height = 0;
	s_width = 320;
	s_height = 0;
	Uint32 fullscreen_flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
	if (!fullscreen) {
		fullscreen_flag = 0;
	}
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		printf("ERROR SDL_Init");
		return;
	}
	SDL_DisplayMode current;
	double current_w = 0;
	double current_h = 0;
	for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
		int should_be_zero = SDL_GetCurrentDisplayMode(i, &current);
		if (should_be_zero != 0) {
			printf("Could not get display mode for video display #%d: %s\n", i, SDL_GetError());
		}
		else {
			printf("Display #%d: current display mode is %dx%dpx @ %dhz.\n", i, current.w, current.h, current.refresh_rate);
			current_w = current.w;
			current_h = current.h;
		}
	}
	if (fullscreen_flag != 0) {
		/*
			If screen ratio has a lesser width than 16:9 (4:3 or 16:10 for example) increase
			height to prevent stretching. Black bars will be rendered in this case to only show 320x180 in
			the vertical centre of the screen.
		*/
		double ratio = current_h / current_w;
		width = current_w;
		s_height = floor(s_width * ratio);
		height = current_h;
		printf("ratio:%f w:%d h:%d sw:%d sh:%d\n", ratio, width, height, s_width, s_height);
	}
	else {
		s_height = 180;
		height = s_height * 2;
	}
	printf("w:%d h:%d sw:%d sh:%d\n", width, height, s_width, s_height);
	window = SDL_CreateWindow("software-raster-sdl2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_OPENGL | fullscreen_flag);
	if (window != NULL) {
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (renderer != NULL) {
			texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, s_width, s_height);
			SDL_GL_SetSwapInterval(1);
		}
		else {
			printf("renderer is null\n");
		}
	}
	else {
		printf("window is null\n");
	}
}

void render(void) {
	SDL_UpdateTexture(texture, NULL, raster, s_width * sizeof(Uint32));
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);
}

void set_pixel(unsigned int x, unsigned int y, unsigned int color) {
	if (x < 320 && y < 180) {
		raster2d[x][y] = color;
	}
	else {
		printf("set_pixel out of bounds.");
	}
}

int main(int argc, char** argv) {
	sdl2_init_video();
	init_data();
	while (!quit) {
		check_sdl_events(event);
		// get offset size for upper and lower part of screen.
		int offset = (s_height - 180) / 2;
		if (offset < 0) {
			offset = 0;
		}
		// write noise to raster
		for (int x = 0; x < s_width; x++) {
			for (int y = 0; y < s_height - (offset * 2); y++) {
				set_pixel(x, y, rand() * 0.01);
			}
		}
		// convert raster2d to raster with offset
		for (int r_x = 0; r_x < s_width; r_x++) {
			for (int r_y = 0; r_y < s_height; r_y++) {
				int p_y = r_y + offset;
				if (p_y < s_height) {
					raster[r_x + p_y * s_width] = raster2d[r_x][r_y];
				}
			}
		}
		// render black bar if space left.
		for (int r_x = 0; r_x < s_width; r_x++) {
			for (int r_y = s_height - offset; r_y < s_height; r_y++) {
				raster[r_x + r_y * s_width] = 0;
			}
		}
		render();
		SDL_Delay(16);
	}
	// clean up
	free(raster);
	for (int i = 0; i < s_width; i++) {
		free(raster2d[i]);
	}
	free(raster2d);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
