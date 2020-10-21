package org.restrpc.client;

import java.io.IOException;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;

public class NativeRpcClient implements RpcClient {

    static {
        JniUtils.loadLibrary("restrpc_jni");
    }

    private long rpcClientPointer = -1;

    private Codec codec;

    // The map to cache return type.
    private ConcurrentHashMap<Long, String> localFutureReturnTypenameCache = new ConcurrentHashMap<>();

    private ConcurrentHashMap<Long, RestFuture<?>> localFutureCache = new ConcurrentHashMap<>();

    public NativeRpcClient() {
        rpcClientPointer = nativeNewRpcClient();
    }

    public void connect(String serverAddress) {
        if (rpcClientPointer == -1) {
            throw new RuntimeException("no init");
        }
        nativeConnect(rpcClientPointer, serverAddress);
        codec = new Codec();
    }

    public AsyncRpcFunction asyncFunc(String funcName) {
        if (funcName == null) {
            throw new NullPointerException("Rpc function name should be null.");
        }
        return new AsyncRpcFunctionImpl(this, funcName);
    }

    public <ReturnType> RestFuture<ReturnType> invoke(String funcName, Object[] args) {
        if (rpcClientPointer == -1) {
            throw new RuntimeException("no init");
        }

        byte[] encodedBytes = null;
        try {
            encodedBytes = codec.encode(funcName, args);
        } catch (IOException e) {
            throw new RuntimeException("...");
        }

        if (encodedBytes == null) {
            return null;
        }

        final long requestId = nativeInvoke(rpcClientPointer, encodedBytes);
        RestFuture<ReturnType> futureToReturn = new RestFuture<ReturnType>() {};
        localFutureCache.put(requestId, futureToReturn);
        return futureToReturn;
    }

    public void close() {
        if (rpcClientPointer == -1) {
            throw new RuntimeException("no init");
        }

        nativeDestroy(rpcClientPointer);
        this.rpcClientPointer = -1;
    }

    /**
     * The callback that will be invoked once the reuslt of rpc request received.
     * Note that this method will be invoked in JNI.
     */
    private void onResultReceived(long requestId, byte[] encodedReturnValueBytes) throws IOException {
//        if (requestId not is local_cache) {//error}
//        codec.decodeReturnValue(encodedReturnValueBytes);
        System.out.println("-----------oo java-----------");
        System.out.println("result is " + codec.myDecodeInt(encodedReturnValueBytes));
        System.out.println("-----------end java-----------");

        RestFuture<?> future = localFutureCache.get(requestId);
        future.getClass().getGenericSuperclass().getTypeName();
        Object o = codec.decodeSingleValue(
                future.getClass().getGenericSuperclass().getTypeName(), encodedReturnValueBytes);
    }

    private native long nativeNewRpcClient();

    private native void nativeConnect(long rpcClientPointer, String serverAddress);

    private native long nativeInvoke(long rpcClientPointer, byte[] encodedFuncNameAndArgs);

    private native void nativeDestroy(long rpcClientPointer);
}
