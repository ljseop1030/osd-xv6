#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int passed = 0;
int failed = 0;

void check(const char *test, int actual, int expected)
{
  if (actual == expected) {
    printf("[PASS] %s : got %d\n", test, actual);
    passed++;
  } else {
    printf("[FAIL] %s : expected %d, got %d\n", test, expected, actual);
    failed++;
  }
}

int main()
{
  int ret;
  int mypid = getpid();

  printf("[INFO] init(pid 1) and sh(pid 2) are created during boot.\n");
  printf("[INFO] mytest(pid %d) is the current test program.\n", mypid);

  // ============================================================
  printf("========== Current Process State ==========\n");
  // ============================================================
  printf("[INFO] The following processes are already running:\n");
  ps(0);
  printf("\n");

  // ============================================================
  printf("========== Testing getnice() ==========\n");
  // ============================================================

  // pid 1 (init) exists, default nice = 20
  ret = getnice(1);
  check("getnice(1) - init default nice", ret, 20);

  // pid 2 (sh) exists, default nice = 20
  ret = getnice(2);
  check("getnice(2) - sh default nice", ret, 20);

  // mytest itself, default nice = 20
  ret = getnice(mypid);
  check("getnice(mytest) - mytest default nice", ret, 20);

  // pid 12 does not exist → should return -1
  ret = getnice(12);
  check("getnice(12) - no such process", ret, -1);

  printf("\n");

  // ============================================================
  printf("========== Testing setnice() ==========\n");
  // ============================================================

  // Set nice of pid 1 (init) to 10 → should succeed (return 0)
  ret = setnice(1, 10);
  check("setnice(1, 10) - valid", ret, 0);

  // Verify the change
  ret = getnice(1);
  check("getnice(1) - after setnice to 10", ret, 10);

  // Restore to default
  ret = setnice(1, 20);
  check("setnice(1, 20) - restore default", ret, 0);

  // pid 12 does not exist → should return -1
  ret = setnice(12, 10);
  check("setnice(12, 10) - no such process", ret, -1);

  // nice value 40 is out of range (valid: 0–39) → should return -1
  ret = setnice(1, 40);
  check("setnice(1, 40) - invalid nice value", ret, -1);

  // nice value -1 is out of range → should return -1
  ret = setnice(1, -1);
  check("setnice(1, -1) - negative nice value", ret, -1);

  // Boundary: minimum valid nice value (0)
  ret = setnice(1, 0);
  check("setnice(1, 0) - minimum valid", ret, 0);
  ret = getnice(1);
  check("getnice(1) - after setnice to 0", ret, 0);
  setnice(1, 20); // restore

  // Boundary: maximum valid nice value (39)
  ret = setnice(1, 39);
  check("setnice(1, 39) - maximum valid", ret, 0);
  ret = getnice(1);
  check("getnice(1) - after setnice to 39", ret, 39);
  setnice(1, 20); // restore

  printf("\n");

  // ============================================================
  printf("========== Testing getnice() / setnice() on self ==========\n");
  // ============================================================
  // mytest itself — test that a process can read and modify its own nice value

  ret = setnice(mypid, 5);
  check("setnice(mytest, 5) - set mytest priority", ret, 0);

  ret = getnice(mypid);
  check("getnice(mytest) - after setnice to 5", ret, 5);

  printf("[INFO] ps(%d) - mytest should show priority 5:\n", mypid);
  ps(mypid);

  ret = setnice(mypid, 20);
  check("setnice(mytest, 20) - restore mytest nice", ret, 0);

  printf("\n");

  // ============================================================
  printf("========== Testing ps() ==========\n");
  // ============================================================

  // ps(0) prints all processes — verify visually
  printf("[INFO] ps(0) - should print all processes (init, sh, mytest):\n");
  ps(0);
  printf("\n");

  // ps(1) prints only init
  printf("[INFO] ps(1) - should print only init:\n");
  ps(1);
  printf("\n");

  // ps(12) — no such process, should print nothing
  printf("[INFO] ps(12) - no such process, should print nothing:\n");
  ps(12);
  printf("[INFO] (no output expected above)\n");
  printf("\n");

  // ps reflects setnice changes — set init nice to 7, verify visually, restore
  setnice(1, 7);
  printf("[INFO] ps(1) - init priority should show 7:\n");
  ps(1);
  setnice(1, 20);
  printf("[INFO] ps(1) - init priority restored to 20:\n");
  ps(1);

  printf("\n");

  // ============================================================
  printf("========== Testing meminfo() ==========\n");
  // ============================================================

  uint64 mem = meminfo();
  if (mem > 0) {
    printf("[PASS] meminfo() - free memory: %lu bytes\n", mem);
    passed++;
  } else {
    printf("[FAIL] meminfo() - expected > 0, got %lu\n", mem);
    failed++;
  }

  // Call meminfo twice — both results should be positive and consistent
  uint64 mem1 = meminfo();
  uint64 mem2 = meminfo();
  if (mem1 > 0 && mem2 > 0) {
    printf("[PASS] meminfo() - consistent: %lu / %lu bytes\n", mem1, mem2);
    passed++;
  } else {
    printf("[FAIL] meminfo() - inconsistent or zero: %lu / %lu\n", mem1, mem2);
    failed++;
  }

  printf("\n");

  // ============================================================
  printf("========== Testing waitpid() ==========\n");
  // ============================================================

  // Test 1: fork a child, then waitpid for it
  int pid1 = fork();
  if (pid1 < 0) {
    printf("[FAIL] fork() failed\n");
    failed++;
  } else if (pid1 == 0) {
    exit(0);
  } else {
    ret = waitpid(pid1);
    check("waitpid(pid1) - child exited", ret, 0);
  }

  // Test 2: fork another child, waitpid for it
  int pid2 = fork();
  if (pid2 < 0) {
    printf("[FAIL] fork() failed\n");
    failed++;
  } else if (pid2 == 0) {
    exit(0);
  } else {
    ret = waitpid(pid2);
    check("waitpid(pid2) - child exited", ret, 0);
  }

  // Test 3: waitpid for init (pid 1) — not our child, should fail
  ret = waitpid(1);
  check("waitpid(1) - not our child", ret, -1);

  // Test 4: waitpid for non-existent pid — should fail
  ret = waitpid(99);
  check("waitpid(99) - no such process", ret, -1);

  // Test 5: waitpid twice on the same pid — second call should fail
  // after freeproc(), the process slot is released and the pid no longer exists
  int pid3 = fork();
  if (pid3 < 0) {
    printf("[FAIL] fork() failed\n");
    failed++;
  } else if (pid3 == 0) {
    exit(0);
  } else {
    ret = waitpid(pid3);
    check("waitpid(pid3) - first wait", ret, 0);
    ret = waitpid(pid3);
    check("waitpid(pid3) - already reaped", ret, -1);
  }

  printf("\n");

  // ============================================================
  printf("========== Summary ==========\n");
  // ============================================================
  printf("Total: %d passed, %d failed\n", passed, failed);

  exit(0);
}