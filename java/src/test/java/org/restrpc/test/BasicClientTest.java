package org.restrpc.test;

import org.msgpack.core.MessageBufferPacker;
import org.msgpack.core.MessagePack;
import org.msgpack.core.MessageUnpacker;
import org.restrpc.client.Codec;
import org.restrpc.client.NativeRpcClient;
import org.restrpc.client.RestFuture;
import org.restrpc.client.RpcClient;
import org.testng.annotations.Test;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

public class BasicClientTest {

    @Test
    public void testBasic() throws IOException, InterruptedException, ExecutionException {
        RpcClient rpcClient = new NativeRpcClient();
        rpcClient.connect("127.0.0.1:9000");
        CompletableFuture<Object> future = rpcClient.asyncFunc("echo").invoke(String.class, 2, 3);
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

    @Test
    public void testDecode() throws IOException {
//        CompletableFuture<?> i = new CompletableFuture<Integer>() {};
//        System.out.println(i.getClass().getDeclare);
//
//        OutputStream os = new ByteArrayOutputStream();
//        MessageBufferPacker messagePacker = MessagePack.newDefaultBufferPacker();
//        messagePacker.packArrayHeader(3);
//        messagePacker.packString("add");
//        messagePacker.packInt(2);
//        messagePacker.packInt(3);
//        byte[] bs1 = messagePacker.toByteArray();
//
//        MessageUnpacker messageUnpacker = MessagePack.newDefaultUnpacker(bs1);
//        messageUnpacker.unpackInt();
    }
}
