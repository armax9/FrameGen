CPU helper tests only (no graphics device):
  clang++ -std=c++20 -O2 -pthread source_release_phase_cpu_test.cpp -o phase_test
  ./phase_test
  clang++ -std=c++20 -O2 -pthread source_credit_cpu_test.cpp -o credit_test
  ./credit_test
Run from this tests folder. Actual-DLL/GPU fixtures are validated separately.
