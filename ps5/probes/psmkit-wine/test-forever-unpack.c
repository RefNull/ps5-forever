/* Runs forever_unpack_zip for test-forever-unpack.py:
 *   test-forever-unpack ZIP SHA256 MEMBER DESTINATION
 * prints "result=N step=TEXT" and exits 0 only when the member was published. */
#include <stdio.h>
#include "forever-unpack.h"

int main(int argc, char **argv)
{
    const char *step = "";
    int result;
    if (argc != 5) return 2;
    result = forever_unpack_zip(argv[1], argv[2], argv[3], argv[4], &step);
    printf("result=%d step=%s\n", result, result ? step : "");
    return result ? 1 : 0;
}
