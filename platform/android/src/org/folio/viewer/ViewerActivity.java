package org.folio.viewer;

import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintManager;
import android.view.ViewGroup;
import org.json.JSONObject;
import java.io.OutputStream;
import java.util.ArrayList;
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
    private static final int OPEN_PDF = 21, SAVE_PDF = 22;
    private ReaderAccessibilityView readerAccessibility;
    private File exportCopy;
    private final ExecutorService files = Executors.newSingleThreadExecutor();
    private boolean pickerOpen;

    static {
        System.loadLibrary("folio");
    }

    private static native void deliverFile(byte[] path, byte[] name, byte[] error, byte[] locator);
    private static native void deliverText(byte[] value, boolean accepted);
    private static native void deliverNotice(byte[] message);
    private static native void deliverCommand(int command, int element);
    private static native byte[] writePrintPdf(byte[] path, byte[] password, int[] pages, byte[] destination, CancellationSignal cancellation);
    void dispatchReaderCommand(int command, int element) { deliverCommand(command, element); }

    private static byte[] utf8(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // A recreated NativeActivity starts a new renderer and must reopen its PDF.
        openIntent(getIntent());
        ensureAccessibility();
    }

    private void ensureAccessibility() {
        if (readerAccessibility == null) {
            readerAccessibility = new ReaderAccessibilityView(this);
            addContentView(readerAccessibility, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    public void updateAccessibility(byte[] bytes) {
        String json = new String(bytes, StandardCharsets.UTF_8);
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) return;
            ensureAccessibility();
            try { readerAccessibility.update(new JSONObject(json)); } catch (org.json.JSONException ignored) { }
        });
    }

    public void readerHover(byte[] bytes) {
        String[] fields = new String(bytes, StandardCharsets.UTF_8).split(",");
        if (fields.length != 3) return;
        try {
            float x = Float.parseFloat(fields[0]), y = Float.parseFloat(fields[1]);
            int action = Integer.parseInt(fields[2]);
            runOnUiThread(() -> { if (readerAccessibility != null) readerAccessibility.hoverAt(x, y, action); });
        } catch (NumberFormatException ignored) { }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        openIntent(intent);
    }

    private void openIntent(Intent intent) {
        if (Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null) copyPdf(intent.getData(), "");
        else if (Intent.ACTION_SEND.equals(intent.getAction())) {
            Uri stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (stream != null) copyPdf(stream, "");
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
        if (request == SAVE_PDF) {
            File copy = exportCopy; exportCopy = null;
            if (copy == null) return;
            if (result == RESULT_OK && data != null && data.getData() != null) {
                Uri destination = data.getData();
                files.execute(() -> {
                    try (InputStream input = new java.io.FileInputStream(copy); OutputStream output = getContentResolver().openOutputStream(destination, "w")) {
                        if (output == null) throw new IOException("No destination stream");
                        transfer(input, output); fileError("Saved PDF copy.");
                    } catch (IOException | RuntimeException error) { fileError("Could not save the PDF copy."); }
                    finally { copy.delete(); }
                });
            } else copy.delete();
            return;
        }
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
        deliverNotice(utf8(message));
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
            field.setSelectAllOnFocus(true);
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
            field.post(field::selectAll);
        });
    }

    public void copyText(byte[] value) {
        String text = new String(value, StandardCharsets.UTF_8);
        runOnUiThread(() -> ((ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("PDF text", text)));
    }

    public void openLink(byte[] value) {
        Uri uri = Uri.parse(new String(value, StandardCharsets.UTF_8));
        runOnUiThread(() -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW, uri)); }
            catch (ActivityNotFoundException | SecurityException error) { fileError("No app is available to open this link."); }
        });
    }

    private static void transfer(InputStream input, OutputStream output) throws IOException {
        byte[] buffer = new byte[65536];
        int count;
        while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
    }

    public void documentAction(byte[] value) {
        try {
            JSONObject args = new JSONObject(new String(value, StandardCharsets.UTF_8));
            int command = args.getInt("command"), pages = args.getInt("pages");
            String title = args.getString("title"), password = args.optString("password");
            // Open the source now so changing documents cannot invalidate an in-flight copy.
            InputStream source = new java.io.FileInputStream(args.getString("path"));
            files.execute(() -> {
                File snapshot = null;
                try (InputStream input = source) {
                    snapshot = File.createTempFile(command == 12 ? "shared-" : "output-", ".pdf", getCacheDir());
                    try (OutputStream output = new FileOutputStream(snapshot)) { transfer(input, output); }
                    File copy = snapshot;
                    runOnUiThread(() -> finishDocumentAction(command, copy, title, password, pages));
                } catch (IOException | RuntimeException error) {
                    if (snapshot != null) snapshot.delete();
                    fileError("Could not prepare this PDF.");
                }
            });
        } catch (IOException | org.json.JSONException | RuntimeException error) { fileError("Could not prepare this PDF."); }
    }

    private void finishDocumentAction(int command, File copy, String title, String password, int pages) {
        if (isFinishing() || isDestroyed()) { copy.delete(); return; }
        if (command == 12) {
            Uri uri = new Uri.Builder().scheme("content").authority(getPackageName() + ".files")
                .appendPath(copy.getName()).appendQueryParameter("name", title).build();
            Intent share = new Intent(Intent.ACTION_SEND).setType("application/pdf").putExtra(Intent.EXTRA_STREAM, uri)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            share.setClipData(ClipData.newUri(getContentResolver(), title, uri));
            try { startActivity(Intent.createChooser(share, "Share PDF")); }
            catch (ActivityNotFoundException error) { copy.delete(); fileError("No sharing app is available."); }
        } else if (command == 13) {
            if (exportCopy != null) { copy.delete(); fileError("Finish saving the current copy first."); return; }
            exportCopy = copy;
            Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                .setType("application/pdf").putExtra(Intent.EXTRA_TITLE, title);
            try { startActivityForResult(create, SAVE_PDF); }
            catch (ActivityNotFoundException error) { exportCopy = null; copy.delete(); fileError("The Android save dialog is unavailable."); }
        } else if (command == 11) {
            PrintManager manager = (PrintManager) getSystemService(Context.PRINT_SERVICE);
            manager.print(title, new PrintDocumentAdapter() {
                @Override public void onLayout(PrintAttributes oldAttributes, PrintAttributes attributes, CancellationSignal cancellation,
                                               LayoutResultCallback callback, Bundle extras) {
                    if (cancellation.isCanceled()) { callback.onLayoutCancelled(); return; }
                    callback.onLayoutFinished(new PrintDocumentInfo.Builder(title).setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT)
                        .setPageCount(pages).build(), !attributes.equals(oldAttributes));
                }
                @Override public void onWrite(PageRange[] ranges, ParcelFileDescriptor destination, CancellationSignal cancellation, WriteResultCallback callback) {
                    files.execute(() -> {
                        File output = null;
                        try {
                            if (cancellation.isCanceled()) { callback.onWriteCancelled(); return; }
                            ArrayList<Integer> wanted = new ArrayList<>();
                            for (int i = 0; i < pages; i++) for (PageRange range : ranges) if (i >= range.getStart() && i <= range.getEnd()) { wanted.add(i); break; }
                            int[] selected = new int[wanted.size()];
                            for (int i = 0; i < selected.length; i++) selected[i] = wanted.get(i);
                            output = File.createTempFile("print-", ".pdf", getCacheDir());
                            byte[] error = writePrintPdf(utf8(copy.getAbsolutePath()), utf8(password), selected, utf8(output.getAbsolutePath()), cancellation);
                            if (cancellation.isCanceled()) { callback.onWriteCancelled(); return; }
                            if (error == null || error.length > 0) {
                                callback.onWriteFailed(error == null ? "Could not prepare print pages." : new String(error, StandardCharsets.UTF_8)); return;
                            }
                            try (InputStream input = new java.io.FileInputStream(output); OutputStream stream = new FileOutputStream(destination.getFileDescriptor())) {
                                transfer(input, stream);
                            }
                            if (cancellation.isCanceled()) callback.onWriteCancelled(); else callback.onWriteFinished(ranges);
                        } catch (IOException | RuntimeException error) { callback.onWriteFailed("Could not write print pages."); }
                        finally { if (output != null) output.delete(); }
                    });
                }
                @Override public void onFinish() { copy.delete(); }
            }, null);
        } else copy.delete();
    }

    @Override
    protected void onDestroy() {
        files.shutdown();
        if (exportCopy != null) { exportCopy.delete(); exportCopy = null; }
        super.onDestroy();
    }
}
