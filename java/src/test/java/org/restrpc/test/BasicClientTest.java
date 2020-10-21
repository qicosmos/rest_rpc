package org.restrpc.test;

import org.restrpc.client.NativeRpcClient;
import org.restrpc.client.RpcClient;
import org.testng.Assert;
import org.testng.annotations.Test;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;

public class BasicClientTest {

    @Test
    public void testBasic() throws InterruptedException, ExecutionException {
        // Note that you must run the `basic_server` first and then run this test.
        RpcClient rpcClient = new NativeRpcClient();
        rpcClient.connect("127.0.0.1:9000");
        CompletableFuture<Object> future = rpcClient.asyncFunc("add").invoke(Integer.class, 2, 3);
        Assert.assertEquals(future.get(), 5);

        CompletableFuture<Object> future1 = rpcClient.asyncFunc("echo").invoke(String.class, "hello world");
        Assert.assertEquals(future1.get(), "hello world");
    }

}
