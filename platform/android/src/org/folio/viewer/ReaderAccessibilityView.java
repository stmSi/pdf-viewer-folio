package org.folio.viewer;

import android.graphics.Rect;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import org.json.JSONArray;
import org.json.JSONObject;
import java.text.BreakIterator;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Transparent semantic view over the native renderer; it does not consume taps. */
final class ReaderAccessibilityView extends View {
    private final ViewerActivity activity;
    private final Map<Integer, JSONObject> nodes = new LinkedHashMap<>();
    private final AccessibilityManager manager;
    private int focus = -1, hover = -1, cursor;
    private float surfaceWidth = 1, surfaceHeight = 1;
    private String document = "";
    private boolean scrollable;

    ReaderAccessibilityView(ViewerActivity owner) {
        super(owner);
        activity = owner;
        manager = (AccessibilityManager) owner.getSystemService(android.content.Context.ACCESSIBILITY_SERVICE);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        setFocusable(false);
        setBackgroundColor(android.graphics.Color.TRANSPARENT);
    }

    void update(JSONObject snapshot) {
        surfaceWidth = (float) snapshot.optDouble("width", 1);
        scrollable = snapshot.optBoolean("scrollable");
        surfaceHeight = (float) snapshot.optDouble("height", 1);
        String nextDocument = snapshot.optString("document");
        if (!nextDocument.equals(document)) cursor = 0;
        document = nextDocument;
        nodes.clear();
        JSONArray list = snapshot.optJSONArray("nodes");
        if (list != null) for (int i = 0; i < list.length(); i++) {
            JSONObject node = list.optJSONObject(i);
            if (node != null) nodes.put(node.optInt("id"), node);
        }
        if (!document.isEmpty()) {
            JSONArray body = snapshot.optJSONArray("body");
            if (body != null) try {
                JSONObject node = new JSONObject();
                node.put("id", 0).put("text", true).put("enabled", true).put("label", document);
                node.put("x", body.getDouble(0)).put("y", body.getDouble(1));
                node.put("w", body.getDouble(2)).put("h", body.getDouble(3));
                nodes.put(0, node);
            } catch (org.json.JSONException ignored) { }
        }
        if (!nodes.containsKey(focus)) focus = -1;
        if (manager.isEnabled()) sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
    }

    private Rect bounds(JSONObject node) {
        float sx = getWidth() / Math.max(1, surfaceWidth), sy = getHeight() / Math.max(1, surfaceHeight);
        int x = Math.round((float) node.optDouble("x") * sx), y = Math.round((float) node.optDouble("y") * sy);
        return new Rect(x, y, x + Math.round((float) node.optDouble("w") * sx), y + Math.round((float) node.optDouble("h") * sy));
    }

    private void event(int id, int type) {
        if (!manager.isEnabled() || getParent() == null) return;
        AccessibilityEvent event = AccessibilityEvent.obtain(type);
        event.setPackageName(activity.getPackageName());
        event.setClassName(id == 0 ? "android.widget.TextView" : "android.widget.Button");
        event.setSource(this, id);
        JSONObject node = nodes.get(id);
        if (node != null) event.getText().add(node.optString("label"));
        getParent().requestSendAccessibilityEvent(this, event);
    }

    void hoverAt(float x, float y, int action) {
        if (!manager.isTouchExplorationEnabled()) return;
        int hit = -1;
        if (action != MotionEvent.ACTION_HOVER_EXIT) {
            // Controls precede the document, so document text does not hide links.
            for (Map.Entry<Integer, JSONObject> item : nodes.entrySet())
                if (bounds(item.getValue()).contains((int) x, (int) y)) { hit = item.getKey(); break; }
        }
        if (hit != hover) {
            if (hover != -1) event(hover, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
            hover = hit;
            if (hover != -1) event(hover, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
        }
    }

    @Override public boolean dispatchHoverEvent(MotionEvent event) {
        if (!manager.isTouchExplorationEnabled()) return super.dispatchHoverEvent(event);
        hoverAt(event.getX(), event.getY(), event.getActionMasked());
        return true;
    }

    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return provider; }
    private final AccessibilityNodeProvider provider = new AccessibilityNodeProvider() {
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
            if (id == HOST_VIEW_ID) {
                AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(ReaderAccessibilityView.this);
                ReaderAccessibilityView.super.onInitializeAccessibilityNodeInfo(info);
                info.setClassName("android.widget.FrameLayout");
                info.setContentDescription("PDF reader");
                info.setScrollable(scrollable);
                if (scrollable) { info.addAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD); info.addAction(AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD); }
                for (Integer child : nodes.keySet()) info.addChild(ReaderAccessibilityView.this, child);
                return info;
            }
            JSONObject node = nodes.get(id);
            if (node == null) return null;
            AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain();
            info.setSource(ReaderAccessibilityView.this, id);
            info.setParent(ReaderAccessibilityView.this);
            info.setPackageName(activity.getPackageName());
            boolean text = node.optBoolean("text");
            info.setClassName(text ? "android.widget.TextView" : "android.widget.Button");
            info.setText(node.optString("label"));
            info.setEnabled(node.optBoolean("enabled", true));
            info.setSelected(node.optBoolean("selected"));
            info.setFocusable(true);
            info.setAccessibilityFocused(focus == id);
            info.setVisibleToUser(isShown());
            Rect box = bounds(node);
            info.setBoundsInParent(box);
            int[] screen = new int[2]; getLocationOnScreen(screen);
            box.offset(screen[0], screen[1]); info.setBoundsInScreen(box);
            info.addAction(focus == id ? AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS : AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
            if (!text) { info.setClickable(true); info.addAction(AccessibilityNodeInfo.ACTION_CLICK); }
            else {
                info.setScrollable(true);
                info.addAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD);
                info.addAction(AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD);
                info.addAction(AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY);
                info.addAction(AccessibilityNodeInfo.ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY);
                info.setMovementGranularities(AccessibilityNodeInfo.MOVEMENT_GRANULARITY_CHARACTER |
                    AccessibilityNodeInfo.MOVEMENT_GRANULARITY_WORD | AccessibilityNodeInfo.MOVEMENT_GRANULARITY_LINE |
                    AccessibilityNodeInfo.MOVEMENT_GRANULARITY_PARAGRAPH | AccessibilityNodeInfo.MOVEMENT_GRANULARITY_PAGE);
            }
            return info;
        }
        @Override public AccessibilityNodeInfo findFocus(int type) { return focus == -1 ? null : createAccessibilityNodeInfo(focus); }
        @Override public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(String text, int root) {
            List<AccessibilityNodeInfo> result = new ArrayList<>();
            String needle = text.toLowerCase(Locale.ROOT);
            for (Map.Entry<Integer, JSONObject> item : nodes.entrySet())
                if (item.getValue().optString("label").toLowerCase(Locale.ROOT).contains(needle)) result.add(createAccessibilityNodeInfo(item.getKey()));
            return result;
        }
        @Override public boolean performAction(int id, int action, Bundle arguments) {
            if (id == HOST_VIEW_ID && scrollable && (action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD || action == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD)) {
                activity.dispatchReaderCommand(action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD ? 19 : 20, 0); return true;
            }
            JSONObject node = nodes.get(id);
            if (node == null) return false;
            if (action == AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
                if (focus == id) return false;
                if (focus != -1) event(focus, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                focus = id; event(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED); return true;
            }
            if (action == AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
                if (focus != id) return false;
                event(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED); focus = -1; return true;
            }
            if (action == AccessibilityNodeInfo.ACTION_CLICK && !node.optBoolean("text") && node.optBoolean("enabled", true)) {
                activity.dispatchReaderCommand(16, id); event(id, AccessibilityEvent.TYPE_VIEW_CLICKED); return true;
            }
            if (id == 0 && (action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD || action == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD)) {
                activity.dispatchReaderCommand(action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD ? 17 : 18, 0); return true;
            }
            if (id == 0 && (action == AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY || action == AccessibilityNodeInfo.ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY)) {
                int granularity = arguments == null ? 1 : arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_MOVEMENT_GRANULARITY_INT, 1);
                boolean forward = action == AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY;
                BreakIterator iterator = granularity == AccessibilityNodeInfo.MOVEMENT_GRANULARITY_CHARACTER ? BreakIterator.getCharacterInstance() :
                    granularity == AccessibilityNodeInfo.MOVEMENT_GRANULARITY_WORD ? BreakIterator.getWordInstance() : BreakIterator.getLineInstance();
                iterator.setText(document);
                int next = granularity == AccessibilityNodeInfo.MOVEMENT_GRANULARITY_PAGE ? (forward ? document.length() : 0) :
                    forward ? iterator.following(cursor) : iterator.preceding(cursor);
                if (next == BreakIterator.DONE || next == cursor) return false;
                AccessibilityEvent e = AccessibilityEvent.obtain(AccessibilityEvent.TYPE_VIEW_TEXT_TRAVERSED_AT_MOVEMENT_GRANULARITY);
                e.setSource(ReaderAccessibilityView.this, 0); e.setPackageName(activity.getPackageName());
                e.setClassName("android.widget.TextView"); e.setAction(action); e.setMovementGranularity(granularity);
                e.setFromIndex(Math.min(cursor, next)); e.setToIndex(Math.max(cursor, next)); e.getText().add(document); cursor = next;
                if (getParent() != null) getParent().requestSendAccessibilityEvent(ReaderAccessibilityView.this, e);
                return true;
            }
            return false;
        }
    };
}
