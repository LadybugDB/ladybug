package com.lbugdb;

/**
 * Version is a class to get the version of the Lbug.
 */
public class Version {

    /**
     * Get the version of the Lbug.
     *
     * @return The version of the Lbug.
     */
    public static String getVersion() {
        return Native.lbugGetVersion();
    }

    /**
     * Get the storage version of the Lbug.
     *
     * @return The storage version of the Lbug.
     */
    public static long getStorageVersion() {
        return Native.lbugGetStorageVersion();
    }
}
