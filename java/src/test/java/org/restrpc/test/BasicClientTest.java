package org.restrpc.test;

import org.restrpc.client.NativeRpcClient;
import org.restrpc.client.RpcClient;
import org.testng.annotations.Test;

public class BasicClientTest {

    @Test
    public void testBasic() {
        RpcClient rpcClient = new NativeRpcClient();
        rpcClient.asyncFunc("111").invoke();
    }
}
