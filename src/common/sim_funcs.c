#ifdef SLURM_SIMULATOR

#include "src/common/sim_funcs.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include <err.h>
/* Structures, macros and other definitions */
//#define LIBC_PATH  "/lib/x86_64-linux-gnu/libc.so.6"

#undef DEBUG
//#define DEBUG


typedef struct sim_user_info{
	uid_t sim_uid;
	gid_t sim_gid;
	char *sim_name;
	struct sim_user_info *next;
}sim_user_info_t;

/* Function Pointers */
time_t (*real_time)(time_t *)                                = NULL;

/* Global Variables */
sim_user_info_t * sim_users_list;
int             * slurmctl_pid;     /* Shared Memory */
int             * slurmd_pid;       /* Shared Memory */
char            * global_sync_flag; /* Shared Memory */
int             * trace_recs_end_sim; /* Shared Memory */
int             * slurmd_registered;/* Shared Memory */ /*ANA: Replacing signals with shared vars for slurmd registration ***/
char            * users_sim_path = NULL;

extern void         * timemgr_data;  /* Shared Memory */
extern unsigned int * current_sim;   /* Shared Memory */
extern unsigned int * current_micro; /* Shared Memory */
extern char         * default_slurm_config_file;


/* Function Prototypes */
static void init_funcs();
void init_shared_memory_if_needed();
static int getting_simulation_users();

time_t time(time_t *t) {
	init_shared_memory_if_needed();
	/* If the current_sim pointer is NULL that means that there is no
	 * shared memory segment, at least not yet, therefore use real function
	 * for now.
	 * Note, here we are examing the location of to where the pointer points
	 *       and not the value itself.
	 */
	if (!(current_sim) && !real_time) init_funcs();
	if (!(current_sim)) {
		return real_time(t);
	} else {
		if(t) {
			*t = *(current_sim);}
		return *(current_sim);
	}
};
int gettimeofday(struct timeval *restrict tv, struct timezone *restrict tz){
	init_shared_memory_if_needed();

	if (!(current_sim) && !real_gettimeofday) init_funcs();
	if (!(current_sim)) {
		return real_gettimeofday(tv, tz);
	} else {
		tv->tv_sec       = *(current_sim);
		*(current_micro) = *(current_micro) + 100;
		tv->tv_usec      = *(current_micro);
	}

	return 0;
}

void get_semaphores_names(char *sim_sem, char *slurm_sem) {
	char *sim_id = NULL;
	strcpy(sim_sem, SIM_SEM_NAME);
	strcpy(slurm_sem, SLURM_SEM_NAME);
	if ((sim_id = getenv("SLURM_SIM_ID"))) {
		strcat(sim_sem, sim_id);
		strcat(slurm_sem, sim_id);
	}
}

static int build_shared_memory() {
	int fd;

	fd = shm_open(SLURM_SIM_SHM, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		error("Error opening %s", SLURM_SIM_SHM);
		return -1;
	}

	if (ftruncate(fd, 32)) {
		warn("Warning!  Can not truncate shared memory segment.");
	}

	timemgr_data = mmap(0, 32, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if(!timemgr_data){
		debug("mmaping %s file can not be done\n", SLURM_SIM_SHM);
		return -1;
	}

	return 0;

}

/*
 * Slurmctld and slurmd do not really build shared memory but they use that
 * one built by sim_mgr
 */
int attaching_shared_memory() { 
	int fd;

	fd = shm_open(SLURM_SIM_SHM, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd >= 0) {
		if (ftruncate(fd, 32)) {
			warn("Can not truncate shared memory segment.");
		}
		timemgr_data = mmap(0, 32, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	} else {
		build_shared_memory();
	}

	if (!timemgr_data) {
		error("mmaping %s file can not be done", SLURM_SIM_SHM);
		return -1;
	}

	/* Initializing pointers to shared memory */
	current_sim      = timemgr_data + SIM_SECONDS_OFFSET;
	current_micro    = timemgr_data + SIM_MICROSECONDS_OFFSET;
	slurmctl_pid     = timemgr_data + SIM_PTHREAD_SLURMCTL_PID;
	slurmd_pid       = timemgr_data + SIM_PTHREAD_SLURMD_PID;
	global_sync_flag = timemgr_data + SIM_GLOBAL_SYNC_FLAG_OFFSET;
	trace_recs_end_sim = timemgr_data + SIM_TRACE_RECS_END_SIM_OFFSET;
	slurmd_registered = timemgr_data + SIM_SLURMD_REGISTERED_OFFSET; /*ANA: Replacing signals with shared vars for slurmd registration ***/ 

	return 0;
}

static void init_funcs() {
	void* handle;

	if (real_gettimeofday == NULL) {
		debug("Looking for real gettimeofday function");

		handle = dlopen(NULL, RTLD_LOCAL | RTLD_LAZY);
		if (handle == NULL) {
			debug("Error in dlopen %s", dlerror());
			return;
		}

		real_gettimeofday = dlsym( handle, "gettimeofday");
		if (real_gettimeofday == NULL) {
			error("Error: no sleep function found");
			return;
		}
	}

	if (real_time == NULL) {
		debug("Looking for real time function");

		handle = dlopen(NULL, RTLD_LOCAL | RTLD_LAZY);
		if (handle == NULL) {
			error("Error in dlopen: %s", dlerror());
			return;
		}
		real_time = dlsym( handle, "time");
		if (real_time == NULL) {
			error("Error: no sleep function found");
			return;
		}
	}
}

void init_shared_memory_if_needed() {
	if (!(current_sim)) {
		if (attaching_shared_memory() < 0) {
			error("Error attaching/building shared memory and mmaping it");
		};
	}
}

/* User- and uid-related functions */
uid_t sim_getuid(const char *name) {
	sim_user_info_t *aux;

	if (!sim_users_list) getting_simulation_users();

	aux = sim_users_list;
	debug2("sim_getuid: starting search for username %s", name);

	while (aux) {
		if (strcmp(aux->sim_name, name) == 0) {
			debug2("sim_getuid: found uid %u for username %s",
						aux->sim_uid, aux->sim_name);
			debug2("sim_getuid--name: %s uid: %u", name, aux->sim_uid);
			return aux->sim_uid;
		}
		aux = aux->next;
	}

	debug2("sim_getuid--name: %s uid: <Can NOT find uid>", name);
	return -1;
}

char *sim_getname(uid_t uid) {
	sim_user_info_t *aux;
	char *user_name;

	if (!sim_users_list) getting_simulation_users();

	aux = sim_users_list;

	while (aux) {
		if (aux->sim_uid == uid) {
			//user_name = malloc(100);
			//memset(user_name,'\0',100);
			user_name = xstrdup(aux->sim_name);
			return user_name;
		}
		aux = aux->next;
	}

	return NULL;
}

gid_t sim_getgid(uid_t uid) {
	sim_user_info_t *aux;

	if (!sim_users_list) getting_simulation_users();

	aux = sim_users_list;

	while (aux) {
		if (aux->sim_uid == uid) {
			return aux->sim_gid;
		}
		aux = aux->next;
	}
	return -1;
}

int getpwnam_r(const char *name, struct passwd *pwd, 
		char *buf, size_t buflen, struct passwd **result) {

	pwd->pw_uid = sim_getuid(name);
	if (pwd->pw_uid == -1) {
		*result = NULL;
		debug("No user found for name %s", name);
		return ENOENT;
	}
	pwd->pw_name = strdup(name);
	pwd->pw_gid = sim_getgid(pwd->pw_uid);
	pwd->pw_passwd = NULL;
	pwd->pw_gecos = NULL;
	pwd->pw_dir = NULL;
	pwd->pw_shell = NULL;

	debug("Found uid %u for name %s", pwd->pw_uid, pwd->pw_name);

	*result = pwd;

	return 0;
}

int getpwuid_r(uid_t uid, struct passwd *pwd,
		char *buf, size_t buflen, struct passwd **result) {

	pwd->pw_name = sim_getname(uid);

	if (pwd->pw_name == NULL) {
		*result = NULL;
		debug("No user found for uid %u", uid);
		return ENOENT;
	}
	pwd->pw_uid = uid;
	pwd->pw_gid = sim_getgid(uid);
	pwd->pw_passwd = NULL;
	pwd->pw_gecos = NULL;
	pwd->pw_dir = NULL;
	pwd->pw_shell = NULL;

	debug("Found name %s for uid %u", pwd->pw_name, pwd->pw_uid);

	*result = pwd;

	return 0;

}

void determine_users_sim_path() {
	char *ptr = NULL;

	if (!users_sim_path) {
		char *name = getenv("SLURM_CONF");
		if (name) {
			users_sim_path = strdup(name);
		} else {
			users_sim_path = strdup(default_slurm_config_file);
		}

		ptr = strrchr(users_sim_path, '/');
		if (ptr) {
			/* Found a path, truncate the file name */
			++ptr;
			*ptr = '\0';
		} else {
			free(users_sim_path);
			users_sim_path = strdup("./");
		}
	}
}

static int getting_simulation_users() {
	char              username[100], users_sim_file_name[1024];
	char              uid_string[10];
	char			  gid_string[10];
	sim_user_info_t * new_sim_user;
	int               fich, pos;
	char              c;

	if (sim_users_list)
		return 0;

	determine_users_sim_path();
	sprintf(users_sim_file_name, "%s%s", users_sim_path, "users.sim");
	fich = open(users_sim_file_name, O_RDONLY);
	if (fich < 0) {
		info("ERROR: no users.sim available");
		return -1;
	}

	debug("Starting reading users...");

	while (1) {

		memset(&username, '\0', 100);
		pos = 0;

		while (read(fich, &c, 1) > 0) {
			username[pos] = c;
			if (username[pos] == ':') {
				username[pos] = '\0';
				break;
			}
			pos++;
		}

		if (pos == 0)
			break;

		new_sim_user = xmalloc(sizeof(sim_user_info_t));
		if (new_sim_user == NULL) {
			error("Malloc error for new sim user");
			return -1;
		}
		debug("Reading user %s", username);
		new_sim_user->sim_name = xstrdup(username);

		pos = 0;
		memset(&uid_string, '\0', 10);

		while (read(fich, &c, 1) > 0) {
			uid_string[pos] = c;
			if (uid_string[pos] == ':') {
				uid_string[pos] = '\0';
				break;
			}
			pos++;
		}
		debug("Reading uid %s", uid_string);

		new_sim_user->sim_uid = (uid_t)atoi(uid_string);

		pos = 0;
		memset(&gid_string, '\0', 10);

		while (read(fich, &c, 1) > 0) {
			gid_string[pos] = c;
			if (gid_string[pos] == '\n') {
				gid_string[pos] = '\0';
				break;
			}
			pos++;
		}
		debug("Reading gid %s", gid_string);
		new_sim_user->sim_gid = (gid_t)atoi(gid_string);

		/* Inserting as list head */
		new_sim_user->next = sim_users_list;
		sim_users_list = new_sim_user;
	}
	return 0;
}

/*
 * "Constructor" function to be called before the main of each Slurm
 * entity (e.g. slurmctld, slurmd and commands).
 */

void __attribute__ ((constructor)) sim_init(void) {
	void *handle;
#ifdef DEBUG
	sim_user_info_t *debug_list;
#endif

	if (attaching_shared_memory() < 0) {
		error("Error attaching/building shared memory and mmaping it");
	};


	if (getting_simulation_users() < 0) {
		error("Error getting users information for simulation");
	}

#ifdef DEBUG
	debug_list = sim_users_list;
	while (debug_list) {
		info("User %s with uid %u", debug_list->sim_name, debug_list->sim_uid);
		debug_list = debug_list->next;
	}
#endif

	if (real_gettimeofday == NULL) {
		debug("Looking for real gettimeofday function");

		handle = dlopen(NULL, RTLD_LOCAL | RTLD_LAZY);
		if (handle == NULL) {
			error("Error in dlopen %s", dlerror());
			return;
		}

		real_gettimeofday = dlsym( handle, "gettimeofday");
		if (real_gettimeofday == NULL) {
			error("Error: no sleep function found");
			return;
		}
	}

	if (real_time == NULL) {
		debug("Looking for real time function");

		handle = dlopen(NULL, RTLD_LOCAL | RTLD_LAZY);
		if (handle == NULL) {
			error("Error in dlopen: %s", dlerror());
			return;
		}
		real_time = dlsym( handle, "time");
		if (real_time == NULL) {
			error("Error: no sleep function found\n");
			return;
		}
	}

	debug("sim_init: done");
}

/*************ANA: Replacing signals with shared vars for slurmd registration *****/
int
sim_open_sem(char * sem_name, sem_t ** mutex_sync, int max_attempts)
{       
        int iter = 0, max_iter = max_attempts;
        if (!max_iter) max_iter = 10;
        while ((*mutex_sync) == SEM_FAILED && iter < max_iter) {
                (*mutex_sync) = sem_open(sem_name, 0, 0755, 0);
                if ((*mutex_sync) == SEM_FAILED && max_iter > 1) {
                        int err = errno;
                        info("ERROR! Could not open semaphore (%s)-- %s",
                                        sem_name, strerror(err));
                        sleep(1);
                }
                ++iter;
        }
        
        if ((*mutex_sync) == SEM_FAILED)
                return -1;
        else    
                return 0;
}

void
sim_perform_slurmd_register(char * sem_name, sem_t ** mutex_sync)
{
        if (*mutex_sync != SEM_FAILED) {
                sem_wait(*mutex_sync);
        } else {
                while ( *mutex_sync == SEM_FAILED ) {
                        sim_open_sem(sem_name, mutex_sync, 0);
                }
                sem_wait(*mutex_sync);
        }

        *slurmd_registered += 1;
	info("sim_funcs: .. slurmd_registered: %d", *slurmd_registered);
        if(sem_post(*mutex_sync)!=0){
		info("error: in sim_funcs at sem_post when closing semaphore");
	}
}

void
sim_close_sem(sem_t ** mutex_sync)
{
	info("sim_close_sem: closing semaphore");
        if ((*mutex_sync) != SEM_FAILED) {
		//info("sim_close_sem: closing semaphore");
                if(sem_close((*mutex_sync))!=0){
			info("error: in sim_funcs at sem_close when closing semaphore");
		}
        }
}
/**********************************************************************************/

#endif
