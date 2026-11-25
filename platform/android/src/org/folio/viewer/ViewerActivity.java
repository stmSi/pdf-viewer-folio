package org.folio.viewer;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.InputFilter;
import android.text.InputType;
import android.view.WindowManager;
import android.widget.EditText;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ViewerActivity extends NativeActivity {
    private static final int OPEN_PDF = 21;
    private final ExecutorService files = Executors.newSingleThreadExecutor();
    private boolean pickerOpen;

    static {
        System.loadLibrary("folio");
    }

    private static native void deliverFile(byte[] path, byte[] name, byte[] error, byte[] locator);
    private static native void deliverText(byte[] value, boolean accepted);

    private static byte[] utf8(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state == null) openIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        openIntent(intent);
    }

    private void openIntent(Intent intent) {
        if (Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null) {
            copyPdf(intent.getData(), "");
        }
    }

    public void choosePdf() {
        runOnUiThread(() -> {
            if (pickerOpen || isFinishing()) return;
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/pdf");
            try {
                startActivityForResult(intent, OPEN_PDF);
                pickerOpen = true;
            } catch (ActivityNotFoundException error) {
                fileError("The Android document picker is unavailable.");
            }
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != OPEN_PDF) return;
        pickerOpen = false;
        if (result == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            String locator = "";
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                locator = uri.toString();
            } catch (SecurityException ignored) {
                // A provider can grant this selection without granting future access.
            }
            copyPdf(uri, locator);
        } else {
            deliverFile(utf8(""), utf8(""), utf8(""), utf8(""));
        }
    }

    public void reopenPdf(byte[] locatorBytes) {
        String locator = new String(locatorBytes, StandardCharsets.UTF_8);
        Uri uri = Uri.parse(locator);
        runOnUiThread(() -> {
            if (isFinishing()) return;
            if ("content".equals(uri.getScheme())) copyPdf(uri, locator);
            else fileError("Choose this PDF again from Files.");
        });
    }

    private void fileError(String message) {
        deliverFile(utf8(""), utf8(""), utf8(message), utf8(""));
    }

    private String documentName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getString(0);
            }
        } catch (RuntimeException ignored) {
            // Some document providers expose a stream without metadata.
        }
        return "Document.pdf";
    }

    private void copyPdf(Uri uri, String locator) {
        files.execute(() -> {
            File pdf = null;
            try {
                pdf = File.createTempFile("opened-", ".pdf", getCacheDir());
                try (InputStream input = getContentResolver().openInputStream(uri);
                     FileOutputStream output = new FileOutputStream(pdf)) {
                    if (input == null) throw new IOException("Missing input stream");
                    byte[] buffer = new byte[65536];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
                if (isDestroyed() || isFinishing()) {
                    pdf.delete();
                } else {
                    deliverFile(utf8(pdf.getAbsolutePath()), utf8(documentName(uri)), utf8(""), utf8(locator));
                }
            } catch (IOException | RuntimeException error) {
                if (pdf != null) pdf.delete();
                if (!isDestroyed() && !isFinishing()) {
                    fileError("Could not read this PDF. Choose it again from Files.");
                }
            }
        });
    }

    public void requestText(byte[] labelBytes, byte[] initialBytes, int input) {
        String label = new String(labelBytes, StandardCharsets.UTF_8);
        String initial = new String(initialBytes, StandardCharsets.UTF_8);
        runOnUiThread(() -> {
            EditText field = new EditText(this);
            field.setSingleLine(true);
            field.setFilters(new InputFilter[]{new InputFilter.LengthFilter(255)});
            if (input == 2) {
                field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            } else if (input == 1) {
                field.setInputType(InputType.TYPE_CLASS_NUMBER);
            } else {
                field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
            }
            field.setText(initial);
            field.selectAll();
            AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(label)
                .setView(field)
                .setNegativeButton("Cancel", (d, which) -> deliverText(utf8(""), false))
                .setPositiveButton("OK", (d, which) -> deliverText(utf8(field.getText().toString()), true))
                .setOnCancelListener(d -> deliverText(utf8(""), false))
                .create();
            field.requestFocus();
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            dialog.show();
        });
    }

    @Override
    protected void onDestroy() {
        files.shutdownNow();
        super.onDestroy();
    }
}
