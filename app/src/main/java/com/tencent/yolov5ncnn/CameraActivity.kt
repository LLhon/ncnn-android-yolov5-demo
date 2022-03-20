package com.tencent.yolov5ncnn

import android.app.Activity
import android.os.Bundle
import android.view.TextureView

/**
 *
 * Created by LLhon on 2022/3/18 19:28.
 */
class CameraActivity : Activity() {

    private lateinit var mTextureView: TextureView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_camera)

        mTextureView = findViewById(R.id.textureView) as TextureView

        val camera2Helper = Camera2Helper.Builder()
            .context(this)
            .previewOn(mTextureView)
            .build()
        camera2Helper.start()
    }
}