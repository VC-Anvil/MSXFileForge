add_executable(msxff_tests
  test_main.cpp test_fat12.cpp test_basic.cpp test_tape.cpp test_rom.cpp test_disk.cpp)
target_link_libraries(msxff_tests PRIVATE msxcore)
add_test(NAME core COMMAND msxff_tests)
set_tests_properties(core PROPERTIES TIMEOUT 120)
add_test(NAME cli_smoke COMMAND ${CMAKE_COMMAND}
  -DMSXFORGE=$<TARGET_FILE:msxforge> -DDSK2DMK=$<TARGET_FILE:dsk2dmk>
  -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_smoke -DDATA=${CMAKE_CURRENT_SOURCE_DIR}/data
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.cmake)
set_tests_properties(cli_smoke PROPERTIES TIMEOUT 120)
