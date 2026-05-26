# Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
# All rights reserved
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required ( VERSION 3.17 )

if (__build_embeddings_included)
	return ()
endif ()
set ( __build_embeddings_included YES )

function(build_embeddings_lib)
	message ( STATUS "Configuring local embeddings build target..." )

	# Set platform-specific library file names
	if(WIN32)
		set(EMBEDDINGS_LIB_FILE_SRC "${EMBEDDINGS_LIB_NAME}.dll")
		set(EMBEDDINGS_LIB_FILE_DST "lib_${EMBEDDINGS_LIB_NAME}.dll")
	elseif(APPLE)
		set(EMBEDDINGS_LIB_FILE_SRC "lib${EMBEDDINGS_LIB_NAME}.dylib")
		set(EMBEDDINGS_LIB_FILE_DST "lib_${EMBEDDINGS_LIB_NAME}.dylib")
	else()
		set(EMBEDDINGS_LIB_FILE_SRC "lib${EMBEDDINGS_LIB_NAME}.so")
		set(EMBEDDINGS_LIB_FILE_DST "lib_${EMBEDDINGS_LIB_NAME}.so")
	endif()

	if (NOT DEFINED CARGO_COMMAND)
		find_program ( CARGO_COMMAND cargo )
		if (NOT CARGO_COMMAND)
			message ( FATAL_ERROR "Cargo command not found. Please install Rust and ensure cargo is in your PATH." )
		endif ()
	endif ()

	# Enable platform-specific features for embeddings.
	set(EMBEDDINGS_CARGO_FEATURE_LIST "")
	if(APPLE)
		list(APPEND EMBEDDINGS_CARGO_FEATURE_LIST "accelerate")
	elseif(WIN32)
		list(APPEND EMBEDDINGS_CARGO_FEATURE_LIST "download-ort")
	elseif(UNIX)
		# MKL provides multi-threaded BLAS on Linux; skip if not available
		execute_process(COMMAND pkg-config --exists mkl-dynamic-lp64-seq RESULT_VARIABLE MKL_FOUND OUTPUT_QUIET ERROR_QUIET)
		if(MKL_FOUND EQUAL 0)
			list(APPEND EMBEDDINGS_CARGO_FEATURE_LIST "mkl")
		endif()
	endif()
	if(EMBEDDINGS_CARGO_FEATURE_LIST)
		list(JOIN EMBEDDINGS_CARGO_FEATURE_LIST "," EMBEDDINGS_CARGO_FEATURES_JOINED)
		set(EMBEDDINGS_CARGO_FEATURES "--features" "${EMBEDDINGS_CARGO_FEATURES_JOINED}")
	endif()

	set(EMBEDDINGS_LIB_SRC_PATH "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_FILE_SRC}")
	set(EMBEDDINGS_LIB_DST_PATH "${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_FILE_DST}")

	file(GLOB_RECURSE EMBEDDINGS_RUST_SOURCES CONFIGURE_DEPENDS
			"${columnar_SOURCE_DIR}/embeddings/*.toml"
			"${columnar_SOURCE_DIR}/embeddings/Cargo.lock"
			"${columnar_SOURCE_DIR}/embeddings/build.rs"
			"${columnar_SOURCE_DIR}/embeddings/src/*.rs"
	)

	set(EMBEDDINGS_VERSION_STAMP "${CMAKE_CURRENT_BINARY_DIR}/embeddings/version.stamp")
	set(EMBEDDINGS_VERSION_STAMP_CONTENT "${GIT_COMMIT_ID}\n${GIT_TIMESTAMP_ID}\n${EMBEDDINGS_CARGO_FEATURES_JOINED}\n")
	if (EXISTS "${EMBEDDINGS_VERSION_STAMP}")
		file(READ "${EMBEDDINGS_VERSION_STAMP}" EMBEDDINGS_CURRENT_VERSION_STAMP)
	endif()
	if (NOT EMBEDDINGS_CURRENT_VERSION_STAMP STREQUAL EMBEDDINGS_VERSION_STAMP_CONTENT)
		file(WRITE "${EMBEDDINGS_VERSION_STAMP}" "${EMBEDDINGS_VERSION_STAMP_CONTENT}")
	endif()

	add_custom_command(
			OUTPUT "${EMBEDDINGS_LIB_DST_PATH}"
			COMMAND ${CMAKE_COMMAND} -E env "GIT_COMMIT_ID=${GIT_COMMIT_ID}" "GIT_TIMESTAMP_ID=${GIT_TIMESTAMP_ID}"
				${CARGO_COMMAND} build --manifest-path "${columnar_SOURCE_DIR}/embeddings/Cargo.toml" --lib --release ${EMBEDDINGS_CARGO_FEATURES} --target-dir "${CMAKE_CURRENT_BINARY_DIR}/embeddings"
			COMMAND ${CMAKE_COMMAND}
				-DEMBEDDINGS_LIB_SRC_PATH=${EMBEDDINGS_LIB_SRC_PATH}
				-DEMBEDDINGS_LIB_DST_PATH=${EMBEDDINGS_LIB_DST_PATH}
				-DEMBEDDINGS_PDB_SRC_PATH=${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/${EMBEDDINGS_LIB_NAME}.pdb
				-DEMBEDDINGS_PDB_DST_PATH=${CMAKE_CURRENT_BINARY_DIR}/embeddings/release/lib_${EMBEDDINGS_LIB_NAME}.pdb
				-P "${columnar_SOURCE_DIR}/cmake/copy_embeddings_artifacts.cmake"
			DEPENDS ${EMBEDDINGS_RUST_SOURCES} "${EMBEDDINGS_VERSION_STAMP}"
			COMMENT "Building manticoresearch text embeddings library"
			VERBATIM
	)

	if (NOT TARGET manticore_knn_embeddings)
		add_custom_target(manticore_knn_embeddings ALL DEPENDS "${EMBEDDINGS_LIB_DST_PATH}")
	endif()

	set(EMBEDDINGS_LIB "${EMBEDDINGS_LIB_DST_PATH}" PARENT_SCOPE)
	set(MANTICORE_KNN_EMBEDDINGS_LIB "${EMBEDDINGS_LIB_DST_PATH}" CACHE INTERNAL "Path to manticoresearch text embeddings library" FORCE)
endfunction ()
