#include <stdio.h>
#include <string.h>

int main(void) {
    char str[] = "apple,banana,cherry,date";
    const char delim[] = ",";
    char *token;
    char *saveptr;

    token = strtok_r(str, delim, &saveptr);

    while (token != NULL) {
        printf("%s\n", token);
        token = strtok_r(NULL, delim, &saveptr);
    }

    return 0;
}