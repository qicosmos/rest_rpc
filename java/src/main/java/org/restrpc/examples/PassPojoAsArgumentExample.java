package org.restrpc.examples;

import org.restrpc.client.NativeRpcClient;
import org.restrpc.client.RpcClient;

import java.io.Serializable;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

class Person implements Serializable {
    private static final long serialVersionUID = 5989656920203532884L;
    private int id;
    private String name;
    private int age;

    public int getId() {
        return id;
    }

    public void setId(int id) {
        this.id = id;
    }

    public String getName() {
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public int getAge() {
        return age;
    }

    public void setAge(int age) {
        this.age = age;
    }

    @Override
    public String toString() {
        return "Person {" +
                "id=" + id +
                ", name='" + name + '\'' +
                ", age=" + age +
                '}';
    }
}

public class PassPojoAsArgumentExample {

    public static void main(String[] args) throws InterruptedException, ExecutionException {
        /**
         * An example shows how we use this Java client to connect to
         * the C++ RPC server and invoke the C++ RPC methods.
         *
         * First of all, we should run a C++ rpc server. In this example,
         * we first run the `basic_server` which is written here:
         * https://github.com/qicosmos/rest_rpc/blob/master/examples/server/main.cpp
         */
        RpcClient rpcClient = new NativeRpcClient();
        rpcClient.connect("127.0.0.1:9000");

        {
            Person p = new Person();
            p.setId(10001);
            p.setName("Jack");
            p.setAge(22);
            CompletableFuture<?> future = rpcClient.asyncFunc("get_person_name").invoke(String.class, p);
            System.out.println("The result of get_person_name() is " + future.get());
        }

        {
            CompletableFuture<?> future = rpcClient.asyncFunc("get_person").invoke(Person.class);
            System.out.println("The result of get_person() is " + future.get());
        }

        rpcClient.close();
    }
}
