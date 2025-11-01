package com.ladybugdb;

import java.util.Map;
import java.io.File;
import java.io.IOException;
import java.net.URL;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/**
 * Native is a wrapper class for the native library.
 * It is used to load the native library and call the native functions.
 * This class is not intended to be used by end users.
 */
public class Native {
    static {
        try {
            String os_name = "";
            String os_arch;
            String os_name_detect = System.getProperty("os.name").toLowerCase().trim();
            String os_arch_detect = System.getProperty("os.arch").toLowerCase().trim();
            boolean isAndroid = System.getProperty("java.runtime.name", "").toLowerCase().contains("android")
                || System.getProperty("java.vendor", "").toLowerCase().contains("android")
                || System.getProperty("java.vm.name", "").toLowerCase().contains("dalvik");
            switch (os_arch_detect) {
                case "x86_64":
                case "amd64":
                    os_arch = "amd64";
                    break;
                case "aarch64":
                case "arm64":
                    os_arch = "arm64";
                    break;
                case "i386":
                    os_arch = "i386";
                    break;
                default:
                    throw new IllegalStateException("Unsupported system architecture");
            }
            if (isAndroid){
                os_name = "android";
            }
            else if (os_name_detect.startsWith("windows")) {
                os_name = "windows";
            } else if (os_name_detect.startsWith("mac")) {
                os_name = "osx";
            } else if (os_name_detect.startsWith("linux")) {
                os_name = "linux";
            }
            String lib_res_name = "/liblbug_java_native.so" + "_" + os_name + "_" + os_arch;

            Path lib_file = Files.createTempFile("liblbug_java_native", ".so");
            URL lib_res = Native.class.getResource(lib_res_name);
            if (lib_res == null) {
                throw new IOException(lib_res_name + " not found");
            }
            Files.copy(lib_res.openStream(), lib_file, StandardCopyOption.REPLACE_EXISTING);
            new File(lib_file.toString()).deleteOnExit();
            String lib_path = lib_file.toAbsolutePath().toString();
            System.load(lib_path);
            if (os_name.equals("linux")) {
                lbugNativeReloadLibrary(lib_path);
            }
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    // Hack: Reload the native library again in JNI bindings to work around the
    // extension loading issue on Linux as System.load() does not set
    // `RTLD_GLOBAL` flag and there is no way to set it in Java.
    protected static native void lbugNativeReloadLibrary(String libPath);

    // Database
    protected static native long lbugDatabaseInit(String databasePath, long bufferPoolSize,
            boolean enableCompression, boolean readOnly, long maxDbSize, boolean autoCheckpoint,
            long checkpointThreshold,boolean throwOnWalReplayFailure, boolean enableChecksums);

    protected static native void lbugDatabaseDestroy(Database db);

    protected static native void lbugDatabaseSetLoggingLevel(String loggingLevel);

    // Connection
    protected static native long lbugConnectionInit(Database database);

    protected static native void lbugConnectionDestroy(Connection connection);

    protected static native void lbugConnectionSetMaxNumThreadForExec(
            Connection connection, long numThreads);

    protected static native long lbugConnectionGetMaxNumThreadForExec(Connection connection);

    protected static native QueryResult lbugConnectionQuery(Connection connection, String query);

    protected static native PreparedStatement lbugConnectionPrepare(
            Connection connection, String query);

    protected static native QueryResult lbugConnectionExecute(
            Connection connection, PreparedStatement preparedStatement, Map<String, Value> param);

    protected static native void lbugConnectionInterrupt(Connection connection);

    protected static native void lbugConnectionSetQueryTimeout(
            Connection connection, long timeoutInMs);

    // PreparedStatement
    protected static native void lbugPreparedStatementDestroy(PreparedStatement preparedStatement);

    protected static native boolean lbugPreparedStatementIsSuccess(PreparedStatement preparedStatement);

    protected static native String lbugPreparedStatementGetErrorMessage(
            PreparedStatement preparedStatement);

    // QueryResult
    protected static native void lbugQueryResultDestroy(QueryResult queryResult);

    protected static native boolean lbugQueryResultIsSuccess(QueryResult queryResult);

    protected static native String lbugQueryResultGetErrorMessage(QueryResult queryResult);

    protected static native long lbugQueryResultGetNumColumns(QueryResult queryResult);

    protected static native String lbugQueryResultGetColumnName(QueryResult queryResult, long index);

    protected static native DataType lbugQueryResultGetColumnDataType(
            QueryResult queryResult, long index);

    protected static native long lbugQueryResultGetNumTuples(QueryResult queryResult);

    protected static native QuerySummary lbugQueryResultGetQuerySummary(QueryResult queryResult);

    protected static native boolean lbugQueryResultHasNext(QueryResult queryResult);

    protected static native FlatTuple lbugQueryResultGetNext(QueryResult queryResult);

    protected static native boolean lbugQueryResultHasNextQueryResult(QueryResult queryResult);

    protected static native QueryResult lbugQueryResultGetNextQueryResult(QueryResult queryResult);

    protected static native String lbugQueryResultToString(QueryResult queryResult);

    protected static native void lbugQueryResultResetIterator(QueryResult queryResult);

    // FlatTuple
    protected static native void lbugFlatTupleDestroy(FlatTuple flatTuple);

    protected static native Value lbugFlatTupleGetValue(FlatTuple flatTuple, long index);

    protected static native String lbugFlatTupleToString(FlatTuple flatTuple);

    // DataType
    protected static native long lbugDataTypeCreate(
            DataTypeID id, DataType childType, long numElementsInArray);

    protected static native DataType lbugDataTypeClone(DataType dataType);

    protected static native void lbugDataTypeDestroy(DataType dataType);

    protected static native boolean lbugDataTypeEquals(DataType dataType1, DataType dataType2);

    protected static native DataTypeID lbugDataTypeGetId(DataType dataType);

    protected static native DataType lbugDataTypeGetChildType(DataType dataType);

    protected static native long lbugDataTypeGetNumElementsInArray(DataType dataType);

    // Value
    protected static native Value lbugValueCreateNull();

    protected static native Value lbugValueCreateNullWithDataType(DataType dataType);

    protected static native boolean lbugValueIsNull(Value value);

    protected static native void lbugValueSetNull(Value value, boolean isNull);

    protected static native Value lbugValueCreateDefault(DataType dataType);

    protected static native <T> long lbugValueCreateValue(T val);

    protected static native Value lbugValueClone(Value value);

    protected static native void lbugValueCopy(Value value, Value other);

    protected static native void lbugValueDestroy(Value value);

    protected static native Value lbugCreateMap(Value[] keys, Value[] values);

    protected static native Value lbugCreateList(Value[] values);

    protected static native Value lbugCreateList(DataType type, long numElements);

    protected static native long lbugValueGetListSize(Value value);

    protected static native Value lbugValueGetListElement(Value value, long index);

    protected static native DataType lbugValueGetDataType(Value value);

    protected static native <T> T lbugValueGetValue(Value value);

    protected static native String lbugValueToString(Value value);

    protected static native InternalID lbugNodeValGetId(Value nodeVal);

    protected static native String lbugNodeValGetLabelName(Value nodeVal);

    protected static native long lbugNodeValGetPropertySize(Value nodeVal);

    protected static native String lbugNodeValGetPropertyNameAt(Value nodeVal, long index);

    protected static native Value lbugNodeValGetPropertyValueAt(Value nodeVal, long index);

    protected static native String lbugNodeValToString(Value nodeVal);

    protected static native InternalID lbugRelValGetId(Value relVal);

    protected static native InternalID lbugRelValGetSrcId(Value relVal);

    protected static native InternalID lbugRelValGetDstId(Value relVal);

    protected static native String lbugRelValGetLabelName(Value relVal);

    protected static native long lbugRelValGetPropertySize(Value relVal);

    protected static native String lbugRelValGetPropertyNameAt(Value relVal, long index);

    protected static native Value lbugRelValGetPropertyValueAt(Value relVal, long index);

    protected static native String lbugRelValToString(Value relVal);

    protected static native Value lbugCreateStruct(String[] fieldNames, Value[] fieldValues);

    protected static native String lbugValueGetStructFieldName(Value structVal, long index);

    protected static native long lbugValueGetStructIndex(Value structVal, String fieldName);

    protected static native String lbugGetVersion();

    protected static native long lbugGetStorageVersion();
}
