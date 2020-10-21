package org.restrpc.client;


import java.io.IOException;

public class RestFuture<T> {

    private Class<T> metaType;

    private T value;

    public RestFuture(Class<T> metaType) {
        this.metaType = metaType;
    }
    private byte[] encodedData;

    private static Codec codec = new Codec();

    public RestFuture() {
    }

    void complete(Integer value) {
        this.value = (T) value;
    }

    void complete(Long value) {
        this.value = (T) value;
    }

    // ...

    // TODO(qwang): Add uncheck!!!
    public T get() throws IOException {
//        // TODO(qwang): This can be cached.
//        return (T) codec.decodeSingleValue(metaType.getTypeName(), encodedData);
        return null;
    }
}
