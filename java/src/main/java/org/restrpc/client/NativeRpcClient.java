package org.restrpc.client;

import java.io.IOException;
import java.util.HashMap;
import java.util.concurrent.CompletableFuture;

public class NativeRpcClient implements RpcClient {

    static {
        JniUtils.loadLibrary("restrpc_jni");
    }
    private long rpcClientPointer = -1;

    private Codec codec;

    private HashMap<Long, CompletableFuture<Object>> localFutureCache = new HashMap<>();

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

    public CompletableFuture<Object> invoke(String funcName, Object[] args) {
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
        CompletableFuture<Object> futureToReturn = new CompletableFuture<>();
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

    private native long nativeNewRpcClient();

    private native void nativeConnect(long rpcClientPointer, String serverAddress);

    private native long nativeInvoke(long rpcClientPointer, byte[] encodedFuncNameAndArgs);

    private native void nativeDestroy(long rpcClientPointer);
}
