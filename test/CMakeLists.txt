#######################################################################UNITTESTS

include( Catch )

set( QUCS-RF-LAYOUT_TESTS
	"${CMAKE_SOURCE_DIR}/test/unit/test.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mcorn.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mcoupled.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mcross.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mgap.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mlin.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mmbend.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mopen.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mrstub.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mstep.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mtee.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/microstrip/test_mvia.cpp"
	"${CMAKE_SOURCE_DIR}/test/unit/test_schparser.cpp"
	)

add_executable( unittest EXCLUDE_FROM_ALL
	${QUCS-RF-LAYOUT_TESTS}
	${QUCS-RF-LAYOUT_SRCS}
	)

target_compile_definitions( unittest
	PRIVATE
	${QT_DEPRECATED_WARNINGS}
	QRFL_VERSION="${PROJECT_VERSION}"
	QRFL_UNITTEST
	)

target_compile_features( unittest
	PRIVATE
	cxx_std_23
	)

target_compile_options( unittest
	PRIVATE
	$<$<CONFIG:Debug>:-Wall>
	$<$<CONFIG:Debug>:-Wextra>
	$<$<CONFIG:Debug>:-fexceptions>
	$<$<CONFIG:Coverage>:--coverage>
	$<$<CONFIG:Coverage>:-O0>
	)

target_include_directories( unittest
	PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	)

target_link_libraries( unittest
	Catch2::Catch
	$<$<CONFIG:Coverage>:-lgcov>
	)

set_target_properties( unittest PROPERTIES
	OUTPUT_NAME "unittest_qucsrflayout"
#	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out"
	)

#######################################################################RUN_TESTS

include( CTest )

add_custom_target( check
	VERBATIM
	COMMAND ${FIXED_SHELL}
	"echo -e \"[----] \\033[36mRunning tests\\033[0m\""
	USES_TERMINAL
	COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
	DEPENDS unittest
	)

add_custom_target( checkall
	VERBATIM
	COMMAND ${FIXED_SHELL}
	"echo -e \"[----] \\033[36mRunning tests\\033[0m\""
	USES_TERMINAL
	COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -V
	DEPENDS unittest
	)

add_test( NAME Unittest
	COMMAND ${FIXED_SHELL}
	"$<TARGET_FILE:unittest> --use-colour=yes 2> /dev/null"
	)

if( CMAKE_BUILD_TYPE MATCHES Coverage )

	add_custom_target( coverage
		COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure Unittest
		COMMAND ${LCOV} -d "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/unittest.dir/__/src" -c -o "${CMAKE_CURRENT_BINARY_DIR}/coverage.tmp.info"
		COMMAND ${LCOV} -q -r "${CMAKE_CURRENT_BINARY_DIR}/coverage.tmp.info" '${CMAKE_CURRENT_BINARY_DIR}/unittest_autogen/*' '/usr/*' -o "${CMAKE_CURRENT_BINARY_DIR}/coverage.info"
		COMMAND ${GENHTML} "${CMAKE_CURRENT_BINARY_DIR}/coverage.info" -o "${CMAKE_CURRENT_BINARY_DIR}/coverage"
		COMMENT "Run unit tests, perform code coverage, generate lcov report"
		DEPENDS unittest
		)

endif( CMAKE_BUILD_TYPE MATCHES Coverage )

if( QRFL_BUILD_MCP )
	find_package( Python3 COMPONENTS Interpreter )
	if( Python3_Interpreter_FOUND )
		add_test( NAME MCPIntegration
			COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/mcp_integration.py"
			"$<TARGET_FILE:qucsrflayout-mcp>" "${CMAKE_SOURCE_DIR}" )
		set_tests_properties( MCPIntegration PROPERTIES TIMEOUT 60 )
	endif()
endif()

if( QRFL_BUILD_MCP AND Python3_Interpreter_FOUND )
	find_program( QRFL_OCTAVE_EXECUTABLE NAMES octave-cli octave )
	if( QRFL_OCTAVE_EXECUTABLE )
		add_test( NAME MCPSimulationIntegration
			COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/mcp_simulation_integration.py"
			"$<TARGET_FILE:qucsrflayout-mcp>" "${QRFL_OCTAVE_EXECUTABLE}" )
		set_tests_properties( MCPSimulationIntegration PROPERTIES TIMEOUT 60 )
	endif()
endif()

set( QRFL_OPENEMS_TEST_PREFIX "" CACHE PATH "OpenEMS installation prefix for the optional actual solver MCP smoke test" )
if( QRFL_BUILD_MCP AND Python3_Interpreter_FOUND AND QRFL_OCTAVE_EXECUTABLE AND QRFL_OPENEMS_TEST_PREFIX )
	add_test( NAME MCPOpenEMSSmoke
		COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/mcp_openems_smoke.py"
		"$<TARGET_FILE:qucsrflayout-mcp>" "${QRFL_OCTAVE_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/octave/openems_smoke.m" "${QRFL_OPENEMS_TEST_PREFIX}" )
	set_tests_properties( MCPOpenEMSSmoke PROPERTIES TIMEOUT 65 )
endif()
