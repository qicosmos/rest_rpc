package org.restrpc.examples;

import org.restrpc.client.AsyncRpcFunction;
import org.restrpc.client.NativeRpcClient;
import org.restrpc.client.RpcClient;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

public class AsyncInvokeExample {

    public static void main(String[] args) throws InterruptedException {
        /**
         * An example shows how we use this Java client to connect to
         * the C++ RPC server and invoke the C++ RPC methods.
         *
         * First of all, we should run a C++ rpc server. In this example,
         * we first run the `basic_server` which is written here:
         * https://github.com/qicosmos/rest_rpc/blob/master/examples/server/main.cpp
         *
         * And make sure the C++ RPC server is listening on the address "127.0.0.1:9000".
         *
         * From the above C++ file, we can know that we registered the method `add()`:
         *        int add(rpc_conn conn, int a, int b) {
         * 		      return a + b;
         *        }
         *
         * So we can create a Java RPC client to invoke that remote method by the following
         * steps below.
         */

        // Create a RPC client instance.
        RpcClient rpcClient = new NativeRpcClient();
        // Connect to the C++ RPC server which is listening on `127.0.0.1:9000`.
        rpcClient.connect("127.0.0.1:9000");
        // Use `asyncFunc()` method as a proxy of the remote function. This can
        // be known as a stub of the remote method.
        AsyncRpcFunction remoteFunc = rpcClient.asyncFunc("add");
        // The first argument indicates the return type you expected to return. It
        // followed by the other arguments that the remote method takes to perform the
        // `add()` method.
        CompletableFuture<Object> future = remoteFunc.invoke(Integer.class, 100, 230);
        // The last, we can use the future as async return value.
        future.whenComplete((obj, exception) -> {
            if (exception != null) {
                System.out.println("Failed to invoke the add(100, 230) with: " + exception.getMessage());
            } else {
                System.out.println("The result of add(100, 230) is " + obj);
            }
        });
        // You also get the return value synchronously:
        //                Object result = future.get();
        TimeUnit.SECONDS.sleep(10);
    }
}
