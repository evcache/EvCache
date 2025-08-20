#include "../include/common.h"
#include "../include/utils.h"
#include "../vm_tools/gpa_hpa.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>

i32 main(i32 argc, char *argv[])
{
    setup_segv_handler();

    i32 debug_started = start_debug_mod();
    if (debug_started == -1) {
        fprintf(stderr, ERR "Failed to open/start debugging module. Exiting.\n");
        return EXIT_FAILURE;
    }

    size_t pages = 1;
    i32 opt;
    while ((opt = getopt(argc, argv, "n:")) != -1) {
        switch (opt) {
        case 'n':
            pages = strtoul(optarg, NULL, 0);
            break;
        default:
            fprintf(stderr, "Usage: %s -n pages\n", argv[0]);
            return 1;
        }
    }

    size_t len = pages * getpagesize();
    char *buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(buf, 0xa, len);
    
    init_l2_info(&l2_info);
    u64 hpa_fp = va_to_hpa(buf); // first page color
    u32 fpc = cache_get_color(hpa_fp, &l2_info);

    u32 matched = 0;
    for (size_t i = 0; i < pages; i++) {
        char* cur_addr = buf + i * PAGE_SIZE;
        u64 hpa = va_to_hpa(cur_addr);
        u32 color_this_addr = cache_get_color(hpa, &l2_info);
        if (color_this_addr == fpc) matched += 1;
        printf("Page %6zu HPA: 0x%lx\n", i, hpa);
    }

    printf(INFO "%u/%lu matched color\n", matched, pages);
    printf("Color of first page: %u\n", fpc);

    printf(INFO "Sleeping. Ctrl+C to cancel.\n");
    sleep(1000000000);

    munmap(buf, len);
    return 0;
}

