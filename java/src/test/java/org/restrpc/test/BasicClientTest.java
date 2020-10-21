package org.restrpc.test;

import org.restrpc.client.Codec;
import org.restrpc.client.NativeRpcClient;
import org.restrpc.client.RpcClient;
import org.testng.annotations.Test;
import java.io.IOException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;

public class BasicClientTest {

    @Test
    public void testBasic() throws IOException, InterruptedException, ExecutionException {
        RpcClient rpcClient = new NativeRpcClient();
        rpcClient.connect("127.0.0.1:9000");
        CompletableFuture<Object> future = rpcClient.asyncFunc("add").invoke(Integer.class, 2, 3);
        System.out.println("The result of add(2, 3) is " + future.get());
    }

    private String getClassStr(Object o) {
        return o.getClass().getName();
    }

    @Test
    public void testClass() {
        int a = 3;
        System.out.println("int -> " + getClassStr(a));
        long b = 3;
        System.out.println("long -> " + getClassStr(b));
        String s = "3";
        System.out.println("string ->" + getClassStr(s));
        String c = null;
        System.out.println("null str ->" + getClassStr(c));
    }

    @Test
    public void testEncode() throws IOException {
        Codec codec = new Codec();
        Object[] args = new Object[] {2, 3};
        byte[] bs = codec.encode("add", args);
    }
}
