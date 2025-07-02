# Remove ptest files that cause QA issues
do_install:append() {
    # Remove .git directories from ptest
    rm -rf ${D}${PTEST_PATH}/json_test_data/.git

    # Or remove ptest entirely
    rm -rf ${D}${PTEST_PATH}
    rm -rf ${D}${libdir}/${BPN}/ptest
}

# Alternative: disable ptest package completely
PTEST_ENABLED = ""

# Or add the missing dependencies
RDEPENDS:${PN}-ptest += "bash perl"
