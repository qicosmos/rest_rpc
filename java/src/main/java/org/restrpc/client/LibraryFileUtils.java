package org.restrpc.client;

import com.google.common.base.Preconditions;
import org.apache.commons.io.FileUtils;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.Paths;

public class LibraryFileUtils {

  public static final String RESTRPC_LIBRARY_NAME = "restrpc_jni";

  public static File getFile(String destDir, String fileName) {
    final File dir = new File(destDir);
    if (!dir.exists()) {
      try {
        FileUtils.forceMkdir(dir);
      } catch (IOException e) {
        throw new RuntimeException("Couldn't make directory: " + dir.getAbsolutePath(), e);
      }
    }
    String lockFilePath = destDir + File.separator + "file_lock";
    try (FileLock ignored = new RandomAccessFile(lockFilePath, "rw")
        .getChannel().lock()) {
      File file = new File(String.format("%s/%s", destDir, fileName));
      if (file.exists()) {
        return file;
      }

      // File does not exist.
      try (InputStream is = LibraryFileUtils.class.getResourceAsStream("/" + fileName)) {
        Preconditions.checkNotNull(is, "{} doesn't exist.", fileName);
        Files.copy(is, Paths.get(file.getCanonicalPath()));
      } catch (IOException e) {
        throw new RuntimeException("Couldn't get temp file from resource " + fileName, e);
      }
      return file;
    } catch (IOException e) {
      throw new RuntimeException(e);
    }
  }

}
