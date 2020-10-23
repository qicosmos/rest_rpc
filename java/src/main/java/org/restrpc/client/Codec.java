package org.restrpc.client;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.msgpack.core.MessageBufferPacker;
import org.msgpack.core.MessagePack;
import org.msgpack.core.MessageUnpacker;
import org.msgpack.jackson.dataformat.JsonArrayFormat;
import org.msgpack.jackson.dataformat.MessagePackFactory;
import java.io.IOException;
import java.nio.Buffer;
import java.nio.ByteBuffer;

public class Codec {

    private final static String INT_TYPE_NAME = "java.lang.Integer";

    private final static String LONG_TYPE_NAME = "java.lang.Long";

    private final static String STRING_TYPE_NAME = "java.lang.String";

    public byte[] encode(String funcName, Object[] args) throws IOException {
        // assert args != nullptr.
        MessageBufferPacker messagePacker = MessagePack.newDefaultBufferPacker();
        messagePacker.packArrayHeader(1 + args.length);
        messagePacker.packString(funcName);

        for (Object arg : args) {
            if (arg == null) {
                messagePacker.packNil();
                continue;
            }

            final String argTypeName = arg.getClass().getName();
            switch (argTypeName) {
                case INT_TYPE_NAME:
                    messagePacker.packInt((int) arg);
                    break;
                case LONG_TYPE_NAME:
                    messagePacker.packLong((long) arg);
                    break;
                case STRING_TYPE_NAME:
                    messagePacker.packString((String) arg);
                    break;
                default:
                    ObjectMapper objectMapper = new ObjectMapper(new MessagePackFactory());
                    objectMapper.setAnnotationIntrospector(new JsonArrayFormat());
                    messagePacker.writePayload(objectMapper.writeValueAsBytes(arg));
            }
        }
        return messagePacker.toByteArray();
    }

    public Object decodeReturnValue(Class<?> returnClz, byte[] encodedBytes) throws IOException {
        if (returnClz == null) {
            throw new RuntimeException("Internal bug.");
        }

        if (encodedBytes == null) {
            return null;
        }

        // TODO(qwang): unpack nil.
        MessageUnpacker messageUnpacker = MessagePack.newDefaultUnpacker(encodedBytes);
        // Unpack unnecessary fields.
        messageUnpacker.unpackArrayHeader();
        messageUnpacker.unpackInt();

        if (Integer.class.equals(returnClz)) {
            return messageUnpacker.unpackInt();
        } else if (Long.class.equals(returnClz)) {
            return messageUnpacker.unpackLong();
        } else if (String.class.equals(returnClz)) {
            return messageUnpacker.unpackString();
        } else {
            final int remainder = (int) (encodedBytes.length - messageUnpacker.getTotalReadBytes());
            byte[] payload = messageUnpacker.readPayload(remainder);
            ObjectMapper objectMapper = new ObjectMapper(new MessagePackFactory());
            objectMapper.setAnnotationIntrospector(new JsonArrayFormat());
            return objectMapper.readValue(payload, returnClz);
        }
    }
}
