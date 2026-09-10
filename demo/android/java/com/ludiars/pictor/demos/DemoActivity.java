package com.ludiars.pictor.demos;

import android.app.Activity;
import android.os.Bundle;
import android.content.Intent;
import android.widget.ArrayAdapter;
import android.widget.ListView;

public final class DemoActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        ListView demos = new ListView(this);
        demos.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1,
            new String[]{"Instanced spheres — Vulkan", "Surface recovery — animated clear"}));
        demos.setOnItemClickListener((parent, view, position, id) -> {
            Intent intent = new Intent(this, RenderActivity.class);
            intent.putExtra("demo", position);
            startActivity(intent);
        });
        setContentView(demos);
    }
}
