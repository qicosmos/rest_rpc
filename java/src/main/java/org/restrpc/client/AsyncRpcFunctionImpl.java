package org.restrpc.client;

import java.util.concurrent.CompletableFuture;

public class AsyncRpcFunctionImpl implements AsyncRpcFunction {

    private RpcClient rpcClient;

    private String funcName;

    public AsyncRpcFunctionImpl(RpcClient rpcClient, String funcName) {
        this.rpcClient = rpcClient;
        this.funcName = funcName;
    }

    public CompletableFuture<Object> invoke(Class returnClz) {
        return internalInvoke(returnClz, new Object[0]);
    }

    public <Arg1Type> CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1) {
        Object[] args = new Object[] {arg1};
        return internalInvoke(returnClz, args);
    }

    public <Arg1Type, Arg2Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2) {
        Object[] args = new Object[] {arg1, arg2};
        return internalInvoke(returnClz, args);
    }

    public <Arg1Type, Arg2Type, Arg3Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3) {
        Object[] args = new Object[] {arg1, arg2, arg3};
        return internalInvoke(returnClz, args);
    }


    public <Arg1Type, Arg2Type, Arg3Type, Arg4Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3, Arg4Type arg4) {
        Object[] args = new Object[] {arg1, arg2, arg3, arg4};
        return internalInvoke(returnClz, args);
    }

    public <Arg1Type, Arg2Type, Arg3Type, Arg4Type, Arg5Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3, Arg4Type arg4, Arg5Type arg5) {
        Object[] args = new Object[] {arg1, arg2, arg3, arg4, arg5};
        return internalInvoke(returnClz, args);
    }

    public <Arg1Type, Arg2Type, Arg3Type, Arg4Type, Arg5Type, Arg6Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3, Arg4Type arg4, Arg5Type arg5, Arg6Type arg6) {
        Object[] args = new Object[] {arg1, arg2, arg3, arg4, arg5, arg6};
        return internalInvoke(returnClz, args);
    }

    private CompletableFuture<Object> internalInvoke(Class returnClz, Object[] args) {
        return rpcClient.invoke(returnClz, funcName, args);
    }
}
