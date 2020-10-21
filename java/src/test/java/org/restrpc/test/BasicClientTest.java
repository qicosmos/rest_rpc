package org.restrpc.test;

import org.checkerframework.checker.units.qual.C;
import org.msgpack.core.MessagePack;
import org.restrpc.client.Codec;
import org.restrpc.client.NativeRpcClient;
import org.restrpc.client.RpcClient;
import org.testng.annotations.Test;

import java.io.IOException;

public class BasicClientTest {

    @Test
    public void testBasic() {
        RpcClient rpcClient = new NativeRpcClient();
        rpcClient.asyncFunc("111").invoke(2, 3);
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
