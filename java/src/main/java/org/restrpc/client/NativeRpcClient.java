package org.restrpc.client;

import java.util.concurrent.CompletableFuture;

public class NativeRpcClient implements RpcClient {

    static {
        JniUtils.loadLibrary("restrpc_jni");
    }
    private long rpcClientPointer = -1;

    public NativeRpcClient() {
        rpcClientPointer = nativeNewRpcClient();
    }

    public void connect(String serverAddress) {
        if (rpcClientPointer == -1) {
            throw new RuntimeException("no init");
        }
        nativeConnect(rpcClientPointer, serverAddress);
    }

    public AsyncRpcFunction asyncFunc(String funcName) {
        if (funcName == null) {
            throw new NullPointerException("Rpc function name should be null.");
        }
        return new AsyncRpcFunctionImpl(this, funcName);
    }

    public CompletableFuture<Object> invoke(Object[] args) {
        if (rpcClientPointer == -1) {
            throw new RuntimeException("no init");
        }

        nativeInvoke(rpcClientPointer, null);
//        return nativeInvoke(rpcClientPointer, );
        return null;
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

    private native long nativeInvoke(long rpcClientPointer, byte[][] encodedFuncNameAndArgs);

    private native void nativeDestroy(long rpcClientPointer);
}
