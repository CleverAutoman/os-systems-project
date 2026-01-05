/* -*- c -*- */

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"
#include <stdio.h>

static char buf[TEST_SIZE];

static int gcd_int(int a, int b) {
  while (b) { int t = a % b; a = b; b = t; }
  return a;
}

void
test_main (void)
{
  char file_name[32];

  /* 1) create N files */
  for (int i = 0; i < LOOP_SIZE; i++) {
    snprintf(file_name, sizeof file_name, "blar-%d", i);
    CHECK(create(file_name, TEST_SIZE), "create \"%s\"", file_name);
  }

  int N = LOOP_SIZE;
  int start = (N > 0) ? (N / 3) : 0;

  /* Pick a step that is coprime with N (simple choice) */
  int step = 97 % N;
  if (step == 0) step = 1;
  while (gcd_int(step, N) != 1) step++;   /* cheap, runs fast for typical N */

  /* 2) arithmetic-progression access */
  for (int i = 0; i < N; i++) {
    int idx = (start + i * step) % N;
    snprintf(file_name, sizeof file_name, "blar-%d", idx);
    check_file(file_name, buf, TEST_SIZE);
  }
}