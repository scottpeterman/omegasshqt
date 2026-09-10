/* tests/win/minarchive/min.c
 *
 * Calls the one exported function. Prints before and after, so a hang is
 * attributable to the call rather than to anything around it.
 */
#include <stdio.h>

extern int min_answer(void);

int main(void) {
    printf("calling into the Go archive...\n");
    fflush(stdout);
    int v = min_answer();
    printf("returned %d\n", v);
    printf("\nthe Go runtime came up and a call completed\n");
    return v == 42 ? 0 : 1;
}
