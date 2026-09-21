# Test kernel only: lose the hardware queues twice, 90 seconds apart.
# Run on a disposable ENA host with an original-kernel boot fallback.
/^ena_tick\(void \*arg\)/ { in_tick = 1 }
in_tick && /^\t\/\* Device promises a keep-alive/ {
    print "\t/* TEST ONLY: erase hardware queues without rebuilding the driver. */"
    print "\tstatic unsigned int injected;"
    print "\tif (injected < 2 && getnsecuptime() >"
    print "\t    SEC_TO_NSEC(90 * (injected + 1))) {"
    print "\t\tint test_error;"
    print "\t\tinjected++;"
    print "\t\ttest_error = ena_com_dev_reset(sc->sc_ena_dev,"
    print "\t\t    ENA_REGS_RESET_GENERIC);"
    print "\t\tprintf(\"%s: TEST hardware queue loss %u: %d\\n\","
    print "\t\t    ENA_DEVNAME(sc), injected, test_error);"
    print "\t}"
    print ""
    in_tick = 0
    inserted++
}
{ print }
END { if (inserted != 1) exit 1 }
