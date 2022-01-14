/* See LICENSE file for copyright and license details. */

#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define BETWEEN(X, A, B)        ((A) <= (X) && (X) <= (B))

void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
char * run_command(const char *cmd);
void timespec_set_ms(struct timespec *ts, int32_t ms);
int32_t timespec_to_ms(struct timespec *ts);
void timespec_diff(struct timespec *res, struct timespec *a, struct timespec *b);