package org.restrpc.client;

import java.util.concurrent.CompletableFuture;

public interface AsyncRpcFunction {

    CompletableFuture<Object> invoke(Class returnClz);

    <Arg1Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1);

    <Arg1Type, Arg2Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2);

    <Arg1Type, Arg2Type, Arg3Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3);


    <Arg1Type, Arg2Type, Arg3Type, Arg4Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3, Arg4Type arg4);

    <Arg1Type, Arg2Type, Arg3Type, Arg4Type, Arg5Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3, Arg4Type arg4, Arg5Type arg5);

    <Arg1Type, Arg2Type, Arg3Type, Arg4Type, Arg5Type, Arg6Type>
    CompletableFuture<Object> invoke(Class returnClz, Arg1Type arg1, Arg2Type arg2, Arg3Type arg3, Arg4Type arg4, Arg5Type arg5, Arg6Type arg6);

}
