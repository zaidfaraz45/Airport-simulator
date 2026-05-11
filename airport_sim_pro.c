/*
 * airport_sim_pro.c
 *
 * - Single runway with emergency priority
 * - Threads represent planes
 * - Single runway protected by a mutex
 * - Semaphore limits airspace capacity
 * - Emergency planes get priority
 * - ncurses UI + logs.txt
 *
 * Compile:
 *   gcc airport_sim_pro.c -o airport_sim_pro -lpthread -lncurses -Wall -O2
 *
 * Run:
 *   ./airport_sim_pro
 */

#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <ncurses.h>
#include <string.h>
#include <time.h>

#define AIRSPACE_LIMIT 4
#define RUNWAY_TIME 3
#define MAX_UI_LOG_LINES 8

typedef enum
{
    PL_LANDING,
    PL_TAKEOFF
} plane_type_t;

typedef struct Plane
{
    int id;
    plane_type_t type;
    int emergency;
    int assigned;
    struct Plane *next;
} Plane;

typedef struct
{
    Plane *head;
    Plane *tail;
    int size;
} Queue;

/* Globals */
static pthread_mutex_t runway_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static sem_t air_sem;

static Queue landing_q = {NULL, NULL, 0};
static Queue takeoff_q = {NULL, NULL, 0};
static Queue emergency_q = {NULL, NULL, 0};

static int NEXT_PLANE_ID = 1;
static int runway_busy = 0;

/* ncurses windows */
static WINDOW *win_header, *win_queues, *win_runway, *win_log;

/* Logs */
static char *log_lines[MAX_UI_LOG_LINES];
static int log_head = 0;
static FILE *logf = NULL;

/* Sleep in milliseconds */
void msleep(long milliseconds)
{
    struct timespec ts;

    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;

    nanosleep(&ts, NULL);
}

/* Append logs */
void append_log(const char *fmt, ...)
{
    pthread_mutex_lock(&log_mutex);

    if (!logf)
    {
        logf = fopen("logs.txt", "a");

        if (!logf)
        {
            perror("fopen");
        }
    }

    va_list ap;
    va_start(ap, fmt);

    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);

    char timestr[16];
    strftime(timestr, sizeof(timestr), "%H:%M:%S", tm_info);

    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    if (logf)
    {
        fprintf(logf, "[%s] %s\n", timestr, msg);
        fflush(logf);
    }

    va_end(ap);

    char *line = malloc(1024);

    if (line)
    {
        snprintf(line, 1024, "[%s] %s", timestr, msg);

        pthread_mutex_lock(&ui_mutex);

        if (log_lines[log_head])
        {
            free(log_lines[log_head]);
        }

        log_lines[log_head] = line;
        log_head = (log_head + 1) % MAX_UI_LOG_LINES;

        pthread_mutex_unlock(&ui_mutex);
    }

    pthread_mutex_unlock(&log_mutex);
}

/* Queue functions */
void enqueue(Queue *q, Plane *p)
{
    p->next = NULL;

    if (!q->tail)
    {
        q->head = q->tail = p;
    }
    else
    {
        q->tail->next = p;
        q->tail = p;
    }

    q->size++;
}

Plane *dequeue(Queue *q)
{
    if (!q->head)
        return NULL;

    Plane *p = q->head;

    q->head = p->next;

    if (!q->head)
    {
        q->tail = NULL;
    }

    p->next = NULL;
    q->size--;

    return p;
}

/* UI setup */
void ui_init()
{
    initscr();
    start_color();
    use_default_colors();

    init_pair(1, COLOR_WHITE, -1);
    init_pair(2, COLOR_RED, -1);
    init_pair(3, COLOR_GREEN, -1);
    init_pair(4, COLOR_YELLOW, -1);

    int h, w;
    getmaxyx(stdscr, h, w);

    int header_h = 3;
    int queues_h = h / 3;
    int runway_h = 5;
    int log_h = h - header_h - queues_h - runway_h - 3;

    win_header = newwin(header_h, w - 2, 1, 1);
    win_queues = newwin(queues_h, w - 2, 1 + header_h, 1);
    win_runway = newwin(runway_h, w - 2, 1 + header_h + queues_h, 1);
    win_log = newwin(log_h, w - 2, 1 + header_h + queues_h + runway_h, 1);

    box(stdscr, 0, 0);
    mvwprintw(stdscr, 0, 3, " Airport Runway Scheduler ");

    refresh();

    box(win_header, 0, 0);
    mvwprintw(win_header, 1, 2,
              "Keys: q=quit  a=landing  t=takeoff  e=emergency");
    wrefresh(win_header);
}

/* Close UI */
void ui_close()
{
    if (logf)
    {
        fclose(logf);
    }

    for (int i = 0; i < MAX_UI_LOG_LINES; i++)
    {
        if (log_lines[i])
        {
            free(log_lines[i]);
        }
    }

    delwin(win_header);
    delwin(win_queues);
    delwin(win_runway);
    delwin(win_log);

    endwin();
}

/* Refresh UI */
void ui_refresh_all()
{
    pthread_mutex_lock(&ui_mutex);

    /* Queues */
    werase(win_queues);
    box(win_queues, 0, 0);

    mvwprintw(win_queues, 0, 2, " Queues ");

    int row = 1;

    pthread_mutex_lock(&queue_mutex);

    mvwprintw(win_queues, row++, 2, "Emergency Queue: ");

    Plane *p = emergency_q.head;

    wattron(win_queues, COLOR_PAIR(2));

    while (p)
    {
        wprintw(win_queues, "E%d ", p->id);
        p = p->next;
    }

    wattroff(win_queues, COLOR_PAIR(2));

    row++;

    mvwprintw(win_queues, row++, 2, "Landing Queue: ");

    p = landing_q.head;

    while (p)
    {
        wprintw(win_queues, "L%d ", p->id);
        p = p->next;
    }

    row++;

    mvwprintw(win_queues, row++, 2, "Takeoff Queue: ");

    p = takeoff_q.head;

    while (p)
    {
        wprintw(win_queues, "T%d ", p->id);
        p = p->next;
    }

    pthread_mutex_unlock(&queue_mutex);

    wrefresh(win_queues);

    /* Runway */
    werase(win_runway);
    box(win_runway, 0, 0);

    mvwprintw(win_runway, 0, 2, " Runway ");

    if (runway_busy)
    {
        wattron(win_runway, COLOR_PAIR(4));
        mvwprintw(win_runway, 2, 4, "Runway Status: OCCUPIED");
        wattroff(win_runway, COLOR_PAIR(4));
    }
    else
    {
        wattron(win_runway, COLOR_PAIR(3));
        mvwprintw(win_runway, 2, 4, "Runway Status: FREE");
        wattroff(win_runway, COLOR_PAIR(3));
    }

    wrefresh(win_runway);

    /* Logs */
    werase(win_log);
    box(win_log, 0, 0);

    mvwprintw(win_log, 0, 2, " Log ");

    int start = (log_head + 1) % MAX_UI_LOG_LINES;

    int r = 1;

    for (int i = 0; i < MAX_UI_LOG_LINES; i++)
    {
        int idx = (start + i) % MAX_UI_LOG_LINES;

        if (log_lines[idx])
        {
            mvwprintw(win_log, r++, 2, "%s", log_lines[idx]);
        }
    }

    wrefresh(win_log);

    pthread_mutex_unlock(&ui_mutex);
}

/* Control tower */
void *control_tower(void *arg)
{
    (void)arg;

    while (1)
    {
        Plane *selected = NULL;

        pthread_mutex_lock(&queue_mutex);

        if (emergency_q.size > 0)
        {
            selected = dequeue(&emergency_q);
        }
        else if (landing_q.size > 0)
        {
            selected = dequeue(&landing_q);
        }
        else if (takeoff_q.size > 0)
        {
            selected = dequeue(&takeoff_q);
        }

        pthread_mutex_unlock(&queue_mutex);

        if (!selected)
        {
            ui_refresh_all();
            msleep(200);
            continue;
        }

        while (1)
        {
            pthread_mutex_lock(&runway_mutex);

            if (!runway_busy)
            {
                runway_busy = 1;

                pthread_mutex_unlock(&runway_mutex);

                break;
            }

            pthread_mutex_unlock(&runway_mutex);

            ui_refresh_all();
            msleep(200);
        }

        selected->assigned = 1;

        append_log(
            "Control Tower assigned runway to Plane %d",
            selected->id);

        ui_refresh_all();

        msleep(150);
    }

    return NULL;
}

/* Plane thread */
void *plane_thread(void *arg)
{
    Plane *p = (Plane *)arg;

    append_log("Plane %d created", p->id);

    sem_wait(&air_sem);

    append_log("Plane %d entered airspace", p->id);

    pthread_mutex_lock(&queue_mutex);

    if (p->emergency)
    {
        enqueue(&emergency_q, p);
    }
    else if (p->type == PL_LANDING)
    {
        enqueue(&landing_q, p);
    }
    else
    {
        enqueue(&takeoff_q, p);
    }

    pthread_mutex_unlock(&queue_mutex);

    ui_refresh_all();

    while (!p->assigned)
    {
        ui_refresh_all();
        msleep(100);
    }

    append_log("Plane %d using runway", p->id);

    pthread_mutex_lock(&runway_mutex);

    sleep(RUNWAY_TIME);

    runway_busy = 0;

    pthread_mutex_unlock(&runway_mutex);

    append_log("Plane %d left runway", p->id);

    sem_post(&air_sem);

    append_log("Plane %d exited airspace", p->id);

    ui_refresh_all();

    free(p);

    return NULL;
}

/* Spawn plane */
void spawn_plane(plane_type_t type, int emergency)
{
    Plane *p = malloc(sizeof(Plane));

    if (!p)
    {
        perror("malloc");
        return;
    }

    p->id = NEXT_PLANE_ID++;
    p->type = type;
    p->emergency = emergency;
    p->assigned = 0;
    p->next = NULL;

    pthread_t th;

    if (pthread_create(&th, NULL, plane_thread, p) != 0)
    {
        perror("pthread_create");
        free(p);
        return;
    }

    pthread_detach(th);
}

/* Auto generator */
void *generator_thread(void *arg)
{
    (void)arg;

    srand(time(NULL));

    while (1)
    {
        int r = rand() % 100;

        if (r < 60)
        {
            spawn_plane(PL_LANDING, 0);
        }
        else if (r < 90)
        {
            spawn_plane(PL_TAKEOFF, 0);
        }
        else
        {
            spawn_plane(PL_LANDING, 1);
        }

        sleep(2);
    }

    return NULL;
}

int main()
{
    sem_init(&air_sem, 0, AIRSPACE_LIMIT);

    for (int i = 0; i < MAX_UI_LOG_LINES; i++)
    {
        log_lines[i] = NULL;
    }

    ui_init();

    pthread_t tower_th;
    pthread_t gen_th;

    pthread_create(&tower_th, NULL, control_tower, NULL);
    pthread_create(&gen_th, NULL, generator_thread, NULL);

    nodelay(stdscr, TRUE);

    int ch;

    while (1)
    {
        ui_refresh_all();

        ch = getch();

        if (ch == 'q')
        {
            break;
        }
        else if (ch == 'a')
        {
            spawn_plane(PL_LANDING, 0);
        }
        else if (ch == 't')
        {
            spawn_plane(PL_TAKEOFF, 0);
        }
        else if (ch == 'e')
        {
            spawn_plane(PL_LANDING, 1);
        }

        msleep(150);
    }

    ui_close();

    sem_destroy(&air_sem);

    return 0;
}
