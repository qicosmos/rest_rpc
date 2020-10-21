package org.restrpc.client;

import java.util.concurrent.CompletableFuture;

public interface RpcClient {

    void connect(String serverAddress);

    AsyncRpcFunction asyncFunc(String funcName);

    <ReturnType> RestFuture<ReturnType> invoke(String funcName, Object[] args);

    void close();
}
