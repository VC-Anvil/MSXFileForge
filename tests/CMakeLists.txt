add_executable(msxff_tests
  test_main.cpp test_fat12.cpp test_basic.cpp test_tape.cpp test_rom.cpp test_disk.cpp)
target_link_libraries(msxff_tests PRIVATE msxcore)
add_test(NAME core COMMAND msxff_tests)
set_tests_properties(core PROPERTIES TIMEOUT 120)
