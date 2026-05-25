# download_tec.cmake
# Ensures the IONEX TEC file is present for the recvbias regression test.
#
# The compressed file is vendored in the repo and looked up as
# ${TESTDATA_DIR}/<basename>.gz (the regression fixture passes tests/data/malib),
# so CI is deterministic and works offline. The network download from CODE
# (Univ. of Bern) is only a fallback for when that vendored copy is absent, and
# it is integrity-checked against the vendored file's known hash.
#
# Usage:
#   cmake -DTESTDATA_DIR=<path/to/data> -P download_tec.cmake

if(NOT DEFINED TESTDATA_DIR)
    message(FATAL_ERROR "download_tec.cmake: TESTDATA_DIR is not defined")
endif()

set(TEC_BASENAME "COD0OPSFIN_20242350000_01D_01H_GIM.INX")
set(TEC_FILE "${TESTDATA_DIR}/${TEC_BASENAME}")
set(TEC_URL  "http://ftp.aiub.unibe.ch/CODE/2024/${TEC_BASENAME}.gz")
set(TEC_GZ   "${TEC_FILE}.gz")

if(EXISTS "${TEC_FILE}")
    message(STATUS "download_tec: already present ${TEC_FILE}")
    return()
endif()

# Prefer the vendored compressed copy; only hit the network if it is missing.
if(EXISTS "${TEC_GZ}")
    message(STATUS "download_tec: using vendored ${TEC_GZ}")
    set(_vendored TRUE)
else()
    # ftp.aiub.unibe.ch is HTTP-only; verify integrity against the known hash of
    # the vendored archive so a corrupted or tampered fallback download fails
    # loudly instead of silently perturbing test results.
    message(STATUS "download_tec: downloading ${TEC_URL}")
    file(DOWNLOAD "${TEC_URL}" "${TEC_GZ}"
        EXPECTED_HASH SHA256=49d8fd7f0aa7082186fa6937bd50ff25edb3ba6b937d19fba3701d35a4e226cb
        STATUS dl_status
        SHOW_PROGRESS
    )
    list(GET dl_status 0 dl_rc)
    list(GET dl_status 1 dl_msg)

    if(NOT dl_rc EQUAL 0)
        file(REMOVE "${TEC_GZ}")
        message(FATAL_ERROR
            "download_tec: download failed (${dl_rc}: ${dl_msg})\n"
            "  URL: ${TEC_URL}")
    endif()
    set(_vendored FALSE)
endif()

# Decompress .gz -> .INX
execute_process(
    COMMAND gzip -dc "${TEC_GZ}"
    OUTPUT_FILE "${TEC_FILE}"
    RESULT_VARIABLE gz_rc
)

# Keep the vendored archive in place; only clean up a freshly downloaded one.
if(NOT _vendored)
    file(REMOVE "${TEC_GZ}")
endif()

if(NOT gz_rc EQUAL 0)
    file(REMOVE "${TEC_FILE}")
    message(FATAL_ERROR "download_tec: decompression failed (exit code ${gz_rc})")
endif()

message(STATUS "download_tec: OK ${TEC_FILE}")
