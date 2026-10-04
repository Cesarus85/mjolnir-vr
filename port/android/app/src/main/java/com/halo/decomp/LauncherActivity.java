package com.halo.decomp;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;

/**
 * Starts the game once its data is in place.
 *
 * The game reads the Xbox game data (the folder holding maps/) from the
 * app's external files directory, /sdcard/Android/data/com.halo.decomp/files.
 * If it is missing, this screen lets the player pick an Xbox disc image of
 * the game (.xiso or .iso, any version) with the system file picker, and
 * copies its maps folder there (XisoExtractor), as the desktop games do; or
 * they can push the maps folder with adb.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_IMAGE = 1;
    private static final int ASK_MICROPHONE = 2;
    private static final String SHARED_DATA_ROOT = "/home/steamos/Documents/HaloCE";

    private File dataRoot;
    private TextView status;
    private ProgressBar progress;
    private Button pick;
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dataRoot = getExternalFilesDir(null);
        // the Steam Frame: Lepton empties the app's own folders whenever it
        // starts again; the game's data lives in the SteamOS user's
        // Documents instead, when it is there (port/android/STEAM_FRAME.md;
        // the same choice as host_main.c's)
        if (new File(SHARED_DATA_ROOT, "maps/ui.map").isFile() && new File(SHARED_DATA_ROOT).canWrite())
            dataRoot = new File(SHARED_DATA_ROOT);
        // created by the app, so that files pushed into it with adb stay
        // readable (a directory adb creates there belongs to the shell user)
        if (dataRoot != null)
            new File(dataRoot, "maps").mkdirs();
        passOnHardwareId();
        passOnInvite(getIntent());
        if (haveData()) {
            if (!askForMicrophone())
                startGame();
            return;
        }
        buildInterface();
    }

    /**
     * The voice chat's microphone (port/android/host/host_voice.c), asked
     * for here, once, on this flat panel, before the game starts: in the
     * game, the question would stop it. Returns true if it asked (the game
     * starts on the answer).
     */
    private boolean askForMicrophone() {
        SharedPreferences preferences = getSharedPreferences("launcher", MODE_PRIVATE);

        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
            || preferences.getBoolean("asked_microphone", false))
            return false;
        preferences.edit().putBoolean("asked_microphone", true).apply();
        requestPermissions(new String[] { Manifest.permission.RECORD_AUDIO }, ASK_MICROPHONE);
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == ASK_MICROPHONE)
            startGame();
    }

    /**
     * An internet play invite link the app was opened with: the game
     * (port/linux/src/p2p.c) picks it up from join_link.txt, whether it is
     * starting now or already running.
     */
    private void passOnInvite(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()) || intent.getData() == null
            || dataRoot == null)
            return;
        // written whole under another name, then renamed: the game never
        // reads it half written
        File partial = new File(dataRoot, "join_link.txt.tmp");
        try (OutputStream out = new FileOutputStream(partial)) {
            out.write(intent.getData().toString().getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            // the link is lost; the player can copy it instead
            partial.delete();
            return;
        }
        if (!partial.renameTo(new File(dataRoot, "join_link.txt")))
            partial.delete();
    }

    /**
     * This device's ANDROID_ID (the app's own: one per app signing key and
     * user, until a factory reset), which native code cannot read: the game
     * (port/linux/src/p2p.c) hashes it from hardware_id.txt into the
     * hardware id a host it joins is told.
     */
    private void passOnHardwareId() {
        String id;

        if (dataRoot == null)
            return;
        try {
            id = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
        } catch (RuntimeException e) {
            return;
        }
        if (id == null || id.isEmpty())
            return;
        File partial = new File(dataRoot, "hardware_id.txt.tmp");
        try (OutputStream out = new FileOutputStream(partial)) {
            out.write(id.getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            partial.delete();
            return;
        }
        if (!partial.renameTo(new File(dataRoot, "hardware_id.txt")))
            partial.delete();
    }

    /**
     * Whether the microphone may be used, for the game (port/linux/src/
     * voice.c), which opens it only then: opening it otherwise would have
     * Android ask in the middle of the game.
     */
    private void passOnMicrophonePermission() {
        if (dataRoot == null)
            return;
        boolean granted =
            checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
        try (OutputStream out = new FileOutputStream(new File(dataRoot, "microphone_permission.txt"))) {
            out.write((granted ? "1" : "0").getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            // the game then does without the microphone
        }
    }

    /** The game data, or the marker file of the VR self-test (host_xr.c). */
    private boolean haveData() {
        return dataRoot != null && (new File(dataRoot, "maps/ui.map").isFile() ||
            new File(dataRoot, "xr_test").isFile());
    }

    private void startGame() {
        passOnMicrophonePermission();
        startActivity(new Intent(this, HaloActivity.class));
        finish();
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
            getResources().getDisplayMetrics());
    }

    private void buildInterface() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(48), dp(24), dp(48), dp(24));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText("Halo needs its game data");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        TextView message = new TextView(this);
        message.setText("Choose an Xbox disc image of Halo: Combat Evolved (an .iso or .xiso file, any "
            + "version) on this device. Its maps folder is copied into the app's storage (about 1.8 GB), "
            + "and you can delete the image afterwards.\n\n"
            + "You can also copy a maps folder from a computer:\n"
            + "adb push <folder with maps>/. " + (dataRoot != null ? dataRoot.getAbsolutePath() : "") + "/");
        message.setTextColor(Color.rgb(200, 205, 210));
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(16), 0, dp(16));
        layout.addView(message);

        pick = new Button(this);
        pick.setText("Choose disc image");
        pick.setOnClickListener(v -> {
            // (disc images have no MIME type of their own: any file, checked
            // when it is read)
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_IMAGE);
        });
        layout.addView(pick, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressLayout = new LinearLayout.LayoutParams(dp(480),
            LinearLayout.LayoutParams.WRAP_CONTENT);
        progressLayout.topMargin = dp(16);
        layout.addView(progress, progressLayout);

        status = new TextView(this);
        status.setTextColor(Color.rgb(160, 200, 160));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        layout.addView(status);

        setContentView(layout);
        pick.requestFocus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // data pushed with adb while this screen was open
        if (pick != null && pick.isEnabled() && haveData())
            startGame();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_IMAGE || resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri image = data.getData();
        pick.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        status.setText("Reading the disc image...");
        new Thread(() -> importImage(image)).start();
    }

    private void report(String text, int permille) {
        handler.post(() -> {
            status.setText(text);
            if (permille >= 0)
                progress.setProgress(permille);
        });
    }

    private void fail(String text) {
        handler.post(() -> {
            status.setText(text);
            progress.setVisibility(View.GONE);
            pick.setEnabled(true);
            pick.requestFocus();
        });
    }

    private void importImage(Uri image) {
        try (ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(image, "r")) {
            if (descriptor == null)
                throw new java.io.IOException("the file could not be opened");
            try (FileInputStream in = new FileInputStream(descriptor.getFileDescriptor())) {
                FileChannel channel = in.getChannel();

                XisoExtractor.extractMaps(channel, dataRoot, (file, done, total) ->
                    report("Extracting maps/" + file + " (" + (done >> 20) + " of " + (total >> 20) + " MB)",
                        total > 0 ? (int) (done * 1000 / total) : 0));
            }
            handler.post(() -> {
                if (haveData()) {
                    startGame();
                } else {
                    fail("The extraction finished but maps/ui.map is missing.");
                }
            });
        } catch (XisoExtractor.ExtractException exception) {
            fail(exception.getMessage());
        } catch (Exception exception) {
            fail("Extracting failed: " + exception.getMessage());
        }
    }
}
