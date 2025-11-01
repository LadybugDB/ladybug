package com.ladybugdb;

/**
 * Utility functions for Value of rel type.
 */
public class ValueRelUtil {

    /**
     * Get the id of the given rel value.
     *
     * @param value: The rel value.
     * @return The id of the given rel value.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static InternalID getID(Value value) {
        value.checkNotDestroyed();
        return Native.lbugRelValGetId(value);
    }

    /**
     * Get src id of the given rel value.
     *
     * @param value: The rel value.
     * @return The src id of the given rel value.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static InternalID getSrcID(Value value) {
        value.checkNotDestroyed();
        return Native.lbugRelValGetSrcId(value);
    }

    /**
     * Get dst id of the given rel value.
     *
     * @param value: The rel value.
     * @return The dst id of the given rel value.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static InternalID getDstID(Value value) {
        value.checkNotDestroyed();
        return Native.lbugRelValGetDstId(value);
    }

    /**
     * Get the label name of the rel value.
     *
     * @param value: The rel value.
     * @return The label name of the rel value.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static String getLabelName(Value value) {
        value.checkNotDestroyed();
        return Native.lbugRelValGetLabelName(value);
    }

    /**
     * Get the property size of the rel value.
     *
     * @param value: The rel value.
     * @return The property size of the rel value.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static long getPropertySize(Value value) {
        value.checkNotDestroyed();
        return Native.lbugRelValGetPropertySize(value);
    }

    /**
     * Get the property name at the given index from the given rel value.
     *
     * @param value: The rel value.
     * @param index: The index of the property.
     * @return The property name at the given index from the given rel value.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static String getPropertyNameAt(Value value, long index) {
        value.checkNotDestroyed();
        return Native.lbugRelValGetPropertyNameAt(value, index);
    }

    /**
     * Get the property value at the given index from the given rel value.
     *
     * @param value: The rel value.
     * @param index: The index of the property.
     * @return The property value at the given index from the given rel value.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static Value getPropertyValueAt(Value value, long index) {
        value.checkNotDestroyed();
        return Native.lbugRelValGetPropertyValueAt(value, index);
    }

    /**
     * Convert the given rel value to string.
     *
     * @param value: The rel value.
     * @return The given rel value in string format.
     * @throws RuntimeException If the rel value has been destroyed.
     */
    public static String toString(Value value) {
        value.checkNotDestroyed();
        return Native.lbugRelValToString(value);
    }
}
