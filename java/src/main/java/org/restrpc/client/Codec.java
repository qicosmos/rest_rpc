package org.restrpc.client;

import org.msgpack.core.MessageBufferPacker;
import org.msgpack.core.MessagePack;
import java.io.IOException;

public class Codec {
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
                case "java.lang.Integer":
                    messagePacker.packInt((int) arg);
                    break;
                case "java.lang.Long":
                    messagePacker.packLong((long) arg);
                    break;
                case "java.lang.String":
                    messagePacker.packString((String) arg);
                    break;
                default:
                    throw new RuntimeException("Unknown type" + argTypeName);
            }
        }
        return messagePacker.toByteArray();
    }
}
