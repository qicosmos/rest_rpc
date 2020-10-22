package org.restrpc.client;

import java.util.concurrent.CompletableFuture;

public interface RpcClient {

    void connect(String serverAddress);

    AsyncRpcFunction asyncFunc(String funcName);

    CompletableFuture<Object> invoke(Class returnClz, String funcName, Object[] args);

    void close();
}
