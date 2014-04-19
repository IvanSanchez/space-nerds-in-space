
#include <SDL/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "mtwist.h"
#include "vertex.h"
#include "snis_graph.h"
#include "graph_dev.h"
#include "quat.h"
#include "material.h"
#include "entity.h"
#include "mesh.h"
#include "stl_parser.h"
#include "mathutils.h"
#include "snis_typeface.h"
#include "opengl_cap.h"
#include "build_info.h"

#define FOV (30.0 * M_PI / 180.0)
#define FPS 60

#define SCREEN_WIDTH 800	/* window width, in pixels */
#define SCREEN_HEIGHT 600	/* window height, in pixels */
#define ASPECT_RATIO (SCREEN_WIDTH/(float)SCREEN_HEIGHT)

static int real_screen_width;
static int real_screen_height;

static int display_frame_stats = 1;

/* Color depth in bits of our window. */
static int bpp;

static struct mesh *snis_read_model(char *filename)
{
	int l = strlen(filename);

	if (strcasecmp(&filename[l - 3], "obj") == 0)
		return read_obj_file(filename);
	else if (strcasecmp(&filename[l - 3], "stl") == 0)
		return read_stl_file(filename);
	else {
		printf("bad filename='%s', filename[l - 3] = '%s'\n",
			filename, &filename[l - 4]);
		return NULL;
	}
}

static char *help_text =
	"MESH VIEWER\n\n"
	"  CONTROLS\n\n"
	"  * MOUSE RIGHT-CLICK DRAG TO ROTATE MODEL\n\n"
	"  * MOUSE SCROLL WHEEL TO ZOOM\n\n"
	"  * MOUSE CONTROL-RIGHT-CLICK DRAG TO ROTATE LIGHT\n\n"
	"  * ESC TO EXIT VIEWER\n\n"
	"PRESS F1 TO EXIT HELP\n";

static void draw_help_text(const char *text)
{
	int line = 0;
	int i, y = 70;
	char buffer[256];
	int buflen = 0;
	int helpmodeline = 0;

	strcpy(buffer, "");

	i = 0;
	do {
		if (text[i] == '\n' || text[i] == '\0') {
			if (line >= helpmodeline && line < helpmodeline + 20) {
				buffer[buflen] = '\0';
				sng_abs_xy_draw_string(buffer, TINY_FONT, 60, y);
				y += 19;
				strcpy(buffer, "");
				buflen = 0;
				line++;
				if (text[i] == '\0')
					break;
				i++;
				continue;
			} else {
				if (line >= helpmodeline + 20)
					break;
			}
		}
		buffer[buflen++] = text[i++];
	} while (1);
}

static void draw_help_screen()
{
	sng_set_foreground(BLACK);
	sng_current_draw_rectangle(1, 50, 50, SCREEN_WIDTH - 100, SCREEN_HEIGHT - 100);
	sng_set_foreground(GREEN);
	sng_current_draw_rectangle(0, 50, 50, SCREEN_WIDTH - 100, SCREEN_HEIGHT - 100);
	draw_help_text(help_text);
}

static void quit(int code)
{
	SDL_Quit();

	exit(code);
}

static int helpmode;
static SDL_Surface *screen;

static void handle_key_down(SDL_keysym *keysym)
{
	switch (keysym->sym) {
	case SDLK_F1:
		helpmode = !helpmode;
		break;
	case SDLK_ESCAPE:
		quit(0);
		break;
	case SDLK_F11:
		SDL_WM_ToggleFullScreen(screen);
		break;
	case SDLK_PAUSE:
		display_frame_stats = (display_frame_stats + 1) % 3;
		break;
	default:
		break;
	}

}

static volatile int isDragging;
static volatile int isDraggingLight;
static union quat last_lobby_orientation = IDENTITY_QUAT_INITIALIZER;
static union quat last_light_orientation = IDENTITY_QUAT_INITIALIZER;
static union quat lobby_orientation = IDENTITY_QUAT_INITIALIZER;
static union quat light_orientation = IDENTITY_QUAT_INITIALIZER;
static int lobby_zoom = 255;

#define MOUSE_HISTORY 5
static volatile float lastx[MOUSE_HISTORY], lasty[MOUSE_HISTORY];
static volatile int last = -1;
static volatile int lastcount = 0;
static int main_da_motion_notify(int x, int y)
{
	float cx, cy, lx, ly;
	union vec3 v1, v2;
	union quat rotation;

	if (!isDragging) {
		lastcount = 0;
	} else {
		if (lastcount < MOUSE_HISTORY) {
			last++;
			lastx[last % MOUSE_HISTORY] =
				2.0 * (((float) x / (float) real_screen_width) - 0.5);
			lasty[last % MOUSE_HISTORY] =
				2.0 * (((float) y / (float) real_screen_height) - 0.5);
			lastcount++;
			return 0;
		}
		lastcount++;
		lx = lastx[(last + 1) % MOUSE_HISTORY];
		ly = lasty[(last + 1) % MOUSE_HISTORY];
		last = (last + 1) % MOUSE_HISTORY;
		cx = 2.0 * (((float) x / (float) real_screen_width) - 0.5);
		cy = 2.0 * (((float) y / (float) real_screen_height) - 0.5);
		lastx[last] = cx;
		lasty[last] = cy;

		v1.v.z = 0;
		v1.v.y = 0;
		v1.v.x = -1.0;
		v2.v.z = cx - lx;
		v2.v.y = cy - ly;
		v2.v.x = -1.0;

		quat_from_u2v(&rotation, &v1, &v2, 0);
		if (isDraggingLight) {
			quat_mul(&light_orientation, &rotation, &last_light_orientation);
			last_light_orientation = light_orientation;
		} else {
			quat_mul(&lobby_orientation, &rotation, &last_lobby_orientation);
			last_lobby_orientation = lobby_orientation;
		}
	}
	return 0;
}

static int main_da_button_press(int button, int x, int y)
{
	if (button == 3) {
		/* start drag */
		isDragging = 1;

		SDLMod mod = SDL_GetModState();
		isDraggingLight = mod & (KMOD_LCTRL | KMOD_RCTRL);

		last = -1.0f;
		lastcount = 0;
	}
	return 0;
}

static int main_da_scroll(int direction)
{
	if (direction)
		lobby_zoom += 10;
	else
		lobby_zoom -= 10;

	if (lobby_zoom < 0)
		lobby_zoom = 0;
	if (lobby_zoom > 255)
		lobby_zoom = 255;
	return 1;
}

static int main_da_button_release(int button, int x, int y)
{
	if (button == 4) {
		main_da_scroll(1);
		return 1;
	} else if (button == 5) {
		main_da_scroll(0);
		return 1;
	}
	last = -1;
	lastcount = 0;

	if (button == 1 && display_frame_stats) {
		if (graph_dev_graph_dev_debug_menu_click(x, y))
			return 1;
	}

	if (button == 3) {
		if (isDragging) {
			/* end drag */
			isDragging = 0;
			isDraggingLight = 0;
		}
	}

	return 1;
}

static int sdl_button_to_int(int sdl_button)
{
	switch (sdl_button) {
	case SDL_BUTTON_LEFT:
		return 1;
	case SDL_BUTTON_MIDDLE:
		return 2;
	case SDL_BUTTON_RIGHT:
		return 3;
	case SDL_BUTTON_WHEELUP:
		return 4;
	case SDL_BUTTON_WHEELDOWN:
		return 5;
	}
	return 0;
}

static void process_events()
{
	/* Our SDL event placeholder. */
	SDL_Event event;

	/* Grab all the events off the queue. */
	while (SDL_PollEvent(&event)) {

		switch (event.type) {
		case SDL_KEYDOWN:
			/* Handle key presses. */
			handle_key_down(&event.key.keysym);
			break;
		case SDL_QUIT:
			/* Handle quit requests (like Ctrl-c). */
			quit(0);
			break;
		case SDL_VIDEORESIZE:
			real_screen_width = event.resize.w;
			real_screen_height = event.resize.h;

			screen = SDL_SetVideoMode(real_screen_width, real_screen_height,
					bpp, SDL_OPENGL | SDL_RESIZABLE);
			sng_set_screen_size(real_screen_width, real_screen_height);
			break;
		case SDL_MOUSEBUTTONDOWN: {
				int button = sdl_button_to_int(event.button.button);
				if (button > 0)
					main_da_button_press(button, event.button.x, event.button.y);
			}
			break;
		case SDL_MOUSEBUTTONUP: {
				int button = sdl_button_to_int(event.button.button);
				if (button > 0)
					main_da_button_release(button, event.button.x, event.button.y);
			}
			break;
		case SDL_MOUSEMOTION:
			main_da_motion_notify(event.motion.x, event.motion.y);
			break;
		}
	}

}


static struct mesh *target_mesh;
static struct mesh *light_mesh;

#define FRAME_INDEX_MAX 10

static void draw_screen()
{
	static double last_frame_time;
	static int frame_index;
	static float frame_rates[FRAME_INDEX_MAX];
	static float frame_times[FRAME_INDEX_MAX];

	double start_time = time_now_double();

	glClearColor(0.0, 0.0, 0.0, 0.0);

	graph_dev_start_frame();

	sng_set_foreground(WHITE);
	sng_abs_xy_draw_string("F1 FOR HELP", NANO_FONT, SCREEN_WIDTH - 100, 10);

	static struct entity_context *cx;
	if (!cx)
		cx = entity_context_new(50, 50);

	float r = target_mesh->radius / tan(FOV / 2.0); /* 50% size for middle zoom */
	float r_cam = r * lobby_zoom / 255.0;
	
	camera_set_parameters(cx, 0.1f, r * 2.2, SCREEN_WIDTH, SCREEN_HEIGHT, FOV);
	camera_set_pos(cx, r_cam, 0, 0);
	camera_look_at(cx, 0, 0, 0);
	camera_assign_up_direction(cx, 0, 1, 0);

	union vec3 light_pos = { { 1.01 * r, 0, 0 } };
	quat_rot_vec_self(&light_pos, &light_orientation);
	set_lighting(cx, light_pos.v.x, light_pos.v.y, light_pos.v.z);

	calculate_camera_transform(cx);

	struct entity *e = add_entity(cx, target_mesh, 0, 0, 0, WHITE);
	update_entity_orientation(e, &lobby_orientation);

	if (isDraggingLight) {
		union vec3 light_dir = { { 0.75 * r_cam, 0, 0 } };
		quat_rot_vec_self(&light_dir, &light_orientation);
		sng_set_foreground(WHITE);
		render_line(cx, light_dir.v.x, light_dir.v.y, light_dir.v.z, 0, 0, 0);

		e = add_entity(cx, light_mesh, light_dir.v.x, light_dir.v.y, light_dir.v.z, WHITE);
	} else {
		e = add_entity(cx, light_mesh, light_pos.v.x, light_pos.v.y, light_pos.v.z, WHITE);
	}

	render_entities(cx);

	remove_all_entity(cx);

	if (helpmode)
		draw_help_screen(0);

	if (display_frame_stats > 0) {
		float avg_frame_rate = 0;
		float avg_frame_time = 0;
		int i;
		for (i = 0; i < FRAME_INDEX_MAX; i++) {
			avg_frame_rate += frame_rates[i];
			avg_frame_time += frame_times[i];
		}
		avg_frame_rate /= (float)FRAME_INDEX_MAX;
		avg_frame_time /= (float)FRAME_INDEX_MAX;

		sng_set_foreground(WHITE);
		char stat_buffer[30];
		sprintf(stat_buffer, "fps %5.2f", 1.0/avg_frame_rate);
		sng_abs_xy_draw_string(stat_buffer, NANO_FONT, 2, 10);
		sprintf(stat_buffer, "t %0.2f ms", avg_frame_time * 1000.0);
		sng_abs_xy_draw_string(stat_buffer, NANO_FONT, 92, 10);
	}
	if (display_frame_stats > 1)
		graph_dev_display_debug_menu_show();

	graph_dev_end_frame();

	glFinish();

	/*
	 * Swap the buffers. This this tells the driver to
	 * render the next frame from the contents of the
	 * back-buffer, and to set all rendering operations
	 * to occur on what was the front-buffer.
	 *
	 * Double buffering prevents nasty visual tearing
	 * from the application drawing on areas of the
	 * screen that are being updated at the same time.
	 */
	SDL_GL_SwapBuffers();

	if (display_frame_stats > 0) {
		double end_time = time_now_double();

		frame_rates[frame_index] = start_time - last_frame_time;
		frame_times[frame_index] = end_time - start_time;
		frame_index = (frame_index + 1) % FRAME_INDEX_MAX;
		last_frame_time = start_time;
	}
}

int main(int argc, char *argv[])
{
	char *filename, *program;
	struct stat statbuf;

	if (argc < 2) {
		printf("%s <mesh_file>\n", argv[0]);
		exit(-1);
	}

	program = argv[0];
	filename = argv[1];

	if (stat(filename, &statbuf) != 0) {
		fprintf(stderr, "%s: %s: %s\n", program, filename, strerror(errno));
		exit(1);
	}

	/* Information about the current video settings. */
	const SDL_VideoInfo *info = NULL;

	/* First, initialize SDL's video subsystem. */
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		/* Failed, exit. */
		fprintf(stderr, "Video initialization failed: %s\n", SDL_GetError());
		quit(1);
	}

	/* Let's get some video information. */
	info = SDL_GetVideoInfo();

	if (!info) {
		/* This should probably never happen. */
		fprintf(stderr, "Video query failed: %s\n", SDL_GetError());
		quit(1);
	}

	/*
	 * Set our width/height to 640/480 (you would
	 * of course let the user decide this in a normal
	 * app). We get the bpp we will request from
	 * the display. On X11, VidMode can't change
	 * resolution, so this is probably being overly
	 * safe. Under Win32, ChangeDisplaySettings
	 * can change the bpp.
	 */
	bpp = info->vfmt->BitsPerPixel;

	/*
	 * Now, we want to setup our requested
	 * window attributes for our OpenGL window.
	 * We want *at least* 5 bits of red, green
	 * and blue. We also want at least a 16-bit
	 * depth buffer.
	 *
	 * The last thing we do is request a double
	 * buffered window. '1' turns on double
	 * buffering, '0' turns it off.
	 *
	 * Note that we do not use SDL_DOUBLEBUF in
	 * the flags to SDL_SetVideoMode. That does
	 * not affect the GL attribute state, only
	 * the standard 2D blitting setup.
	 */
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 1); /* vsync */
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

	/* start again so we can get a fresh new gl context for our window */
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		/* Failed, exit. */
		fprintf(stderr, "Second video initialization failed: %s\n", SDL_GetError());
		quit(1);
	}

	screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, bpp, SDL_OPENGL | SDL_RESIZABLE);
	if (!screen) {
		fprintf(stderr, "Video mode set failed: %s\n", SDL_GetError());
		quit(1);
	}

	real_screen_width = SCREEN_WIDTH;
	real_screen_height = SCREEN_HEIGHT;

	sng_setup_colors(0);

	snis_typefaces_init();
	graph_dev_setup("share/snis/shader");

	sng_set_extent_size(SCREEN_WIDTH, SCREEN_HEIGHT);
	sng_set_screen_size(real_screen_width, real_screen_height);
	sng_set_clip_window(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

	target_mesh = snis_read_model(filename);
	if (!target_mesh) {
		printf("unable to read model file '%s'\n", filename);
		exit(-1);
	}

	light_mesh = mesh_fabricate_billboard(0, 0, 10, 10); //target_mesh->radius / 10.0, target_mesh->radius / 10.0);

	struct material light_material;
	material_init_texture_mapped_unlit(&light_material);
	light_material.billboard_type = MATERIAL_BILLBOARD_TYPE_SCREEN;
	light_material.texture_mapped_unlit.texture_id = graph_dev_load_texture("share/snis/textures/sun.png");
	light_material.texture_mapped_unlit.do_blend = 1;

	light_mesh->material = &light_material;

	const double maxTimeBehind = 0.5;
	double delta = 1.0/(double)FPS;

	unsigned long frame = 0;
	double currentTime = time_now_double();
	double nextTime = currentTime + delta;
	while (1) {
		currentTime = time_now_double();

		if (currentTime - nextTime > maxTimeBehind)
			nextTime = currentTime;

		if (currentTime >= nextTime) {
			nextTime += delta;

			/* Process incoming events. */
			process_events();
			/* Draw the screen. */
			draw_screen();

			if (frame % FPS == 0)
				graph_dev_reload_changed_textures();
			frame++;
		} else {
			double timeToSleep = nextTime-currentTime;
			if (timeToSleep > 0)
				sleep_double(timeToSleep);
		}
	}

	/* Never reached. */
	return 0;
}
