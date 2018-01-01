// Main loader.

#include <types.h>
#include <matrix.h>
#include <modloader.h>
#include <timers.h>
#include <random.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <getopt.h>

static int modcount;

static pthread_mutex_t rmod_lock;
// Usually -1.
static int main_rmod_override = -1;
static int main_rmod_override_argc;
static char* *main_rmod_override_argv;

static int deinit(void) {
	printf("Cleaning up...\n");
	int ret;
	if ((ret = modules_deinit()) != 0)
		return ret;
	if ((ret = matrix_deinit()) != 0)
		return ret;
	if ((ret = timers_deinit()) != 0)
		return ret;
	pthread_mutex_destroy(&rmod_lock);
	if (main_rmod_override != -1)
		timer_free_argv(main_rmod_override_argc, main_rmod_override_argv);
	printf("Goodbye. :(\n");
	return 0;
}

#ifndef PLATFORM_SDL2
static void interrupt(int t) {
	timers_quitting = 1;
}
#endif


static int pick_other(int mymodno, ulong in) {
	pthread_mutex_lock(&rmod_lock);
	if (main_rmod_override != -1) {
		int res = timer_add(in, main_rmod_override, main_rmod_override_argc, main_rmod_override_argv);
		main_rmod_override = -1;
		pthread_mutex_unlock(&rmod_lock);
		return res;
	}
	pthread_mutex_unlock(&rmod_lock);
	int mod = 0;
	if (modcount != 1)
		while (mod == 0) {
			int random = randn(modcount);
			mod = random;

			// Checks after.
			if (mod == mymodno) mod = 0;
			if (strcmp(modules_get(mod)->type, "gfx") != 0) mod = 0;
		}
	return timer_add(in, mod, 0, NULL);
}

void main_force_random(int mnum, int argc, char ** argv) {
	while (!timers_quitting) {
		pthread_mutex_lock(&rmod_lock);
		if (main_rmod_override == -1) {
			main_rmod_override = mnum;
			main_rmod_override_argc = argc;
			main_rmod_override_argv = argv;
			pthread_mutex_unlock(&rmod_lock);
			return;
		}
		pthread_mutex_unlock(&rmod_lock);
		usleep(5000);
	}
	// Used to prevent deadlock.
	timer_free_argv(argc, argv);
}

int usage(char* name) {
	printf("Usage: %s [-o]\n", name);
	printf("\t-o --output: Set output module. Defaults to dummy.\n");
	return 1;
};

static struct option longopts[] = {
	{ "output", required_argument, NULL, 'o' },
	{ NULL,     0,                 NULL, 0},
};

int main(int argc, char* argv[]) {
	int ch;
#ifdef PLATFORM_SDL2
	char outmod[256] = "sdl2";
#elif defined(PLATFORM_RPI)
	char outmod[256] = "rpi_ws2812b";
#else
	char outmod[256] = "dummy"; // dunno.
#endif

	while ((ch = getopt_long(argc, argv, "o:", longopts, NULL)) != -1) {
		switch(ch) {
		case 'o':
			strncpy(outmod, optarg, 255);
			break;
		case '?':
		default:
			return usage(argv[0]);
		}
	}
	argc -= optind;
	argv += optind;

	int ret;
	ret = pthread_mutex_init(&rmod_lock, NULL);
	if (ret) {
		printf("rmod mutex failed to initialize.\n");
		return ret;
	}
	// Initialize Timers.
	ret = timers_init();
	if (ret) {
		printf("Timers failed to initialize.\n");
		pthread_mutex_destroy(&rmod_lock);
		return ret;
	}

	// Initialize pseudo RNG.
	random_seed();

	// Load modules
	int outmodno;
	if (modules_loaddir("./modules/", outmod, &outmodno) != 0)
		deinit();

	// Initialize Matrix.
	ret = matrix_init(outmodno);
	if (ret) {
		// Fail.
		printf("Matrix: Output plugin failed to initialize.\n");
		timers_deinit();
		pthread_mutex_destroy(&rmod_lock);
		return ret;
	}

	// Initialize modules.
	ret = modules_init();
	if (ret) {
		printf("Modules: Init failed.\n");
		return ret;
	}

	modcount = modules_count();

	#ifndef PLATFORM_SDL2
	// Set up the interrupt handler. Note that SDL2 has it's own interrupt handler, so it takes precedence.
	// In both cases, the active sleep is interrupted by some method and timers_quitting is set to 1.
	signal(SIGINT, interrupt);
	#endif

	// Startup.
	pick_other(-1, utime());

	int lastmod = -1;
	while (!timers_quitting) {
		timer tnext = timer_get();
		if (tnext.moduleno == -1) {
			// Queue random.
			pick_other(lastmod, utime() + RANDOM_TIME * T_SECOND);
		} else {
			wait_until(tnext.time);
			module* mod = modules_get(tnext.moduleno);
			if (tnext.moduleno != lastmod) {
				printf("\n>> Now drawing %s", mod->name);
				fflush(stdout);
			} else {
				printf(".");
				fflush(stdout);
			};
			ret = mod->draw(tnext.argc, tnext.argv);
			timer_free_argv(tnext.argc, tnext.argv);
			lastmod = tnext.moduleno;
			if (ret != 0) {
				if (ret == 1) {
					if (lastmod != tnext.moduleno) // not an animation.
						printf("\nModule chose to pass its turn to draw.");
					pick_other(lastmod, utime() + T_MILLISECOND);
				} else {
					eprintf("Module %s failed to draw: Returned %i", mod->name, ret);
					timers_quitting = 1;
					deinit();
					return 7;
				}
			}
		}
	}

	return deinit();
}
