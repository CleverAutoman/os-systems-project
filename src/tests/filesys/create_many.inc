/* -*- c -*- */

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

static char buf[TEST_SIZE];
    
void
test_main (void)
{
  char file_name[32];

  for (int i = 0; i < LOOP_SIZE; i++) {

    snprintf(file_name, sizeof file_name, "blar-%d", i);
    CHECK (create (file_name, TEST_SIZE), "create \"%s\"", file_name);
    // #if 0
    check_file (file_name, buf, TEST_SIZE);
    // #endif
  }
}
