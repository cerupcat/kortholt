package net.simno.kortholt

import android.content.Context
import android.media.AudioManager
import android.os.Process
import androidx.annotation.RawRes
import androidx.core.content.getSystemService
import com.getkeepsafe.relinker.ReLinker
import java.io.File
import java.util.concurrent.atomic.AtomicLong
import kotlin.time.Duration
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.withContext
import net.lingala.zip4j.ZipFile
// Using fully qualified names to avoid conflicts

internal class KortholtPlayer(
    private val context: Context,
    private val dispatcher: CoroutineDispatcher
) : Kortholt.Player {

    private val patchHandle = AtomicLong(NOT_SET)
    private val kortholtHandle = AtomicLong(NOT_SET)

    // Callbacks for receiving messages from Pure Data
    private val floatReceivers = mutableMapOf<String, (Float) -> Unit>()
    private val listReceivers = mutableMapOf<String, (List<Any>) -> Unit>()
    // TODO: Add receiver implementation once PdBase classes are available

    init {
        ReLinker.loadLibrary(context, "pd", VERSION)
        ReLinker.loadLibrary(context, "pdnative", VERSION)
        ReLinker.loadLibrary(context, "kortholt", VERSION)
        // TODO: Set up PdReceiver once classes are available
    }

    override suspend fun openPatch(
        @RawRes patchRes: Int,
        patchName: String,
        extractZip: Boolean
    ) = withContext(dispatcher) {
        runCatching {
            val handle = kortholtHandle.get()
            if (handle == NOT_SET) {
                android.util.Log.w("KortholtPlayer", "Cannot open patch: stream not started")
                return@runCatching false
            }
            
            context.resources.openRawResource(patchRes).use { input ->
                val dir = context.cacheDir
                val patchFile = File(dir, patchName)
                if (extractZip) {
                    dir.mkdirs()
                    val zip = File.createTempFile("temp", ".zip")
                    zip.outputStream().use { output -> input.copyTo(output) }
                    ZipFile(zip).extractAll(dir.absolutePath)
                    zip.delete()
                    
                    // Add the cache directory to Pure Data search path for extracted files
                    nativeAddToSearchPath(handle, dir.absolutePath)
                } else {
                    patchFile.outputStream().use { output -> input.copyTo(output) }
                }
                
                if (patchFile.exists()) {
                    val patchBaseName = patchName.substringBeforeLast('.')
                    val success = nativeOpenPatch(handle, patchBaseName + ".pd", dir.absolutePath)
                    if (success) {
                        patchHandle.set(1L) // Mark as opened
                        android.util.Log.d("KortholtPlayer", "Patch opened successfully: $patchName")
                    } else {
                        android.util.Log.e("KortholtPlayer", "Failed to open patch: $patchName")
                    }
                    success
                } else {
                    android.util.Log.e("KortholtPlayer", "Patch file not found: ${patchFile.absolutePath}")
                    false
                }
            }
        }.getOrDefault(false)
    }

    override suspend fun closePatch() = withContext(dispatcher) {
        runCatching {
            patchHandle.getAndSet(NOT_SET).takeIf { it != NOT_SET }?.let { 
                // TODO: Close patch once PdBase is available
                android.util.Log.d("KortholtPlayer", "Closing patch")
            }
        }.isSuccess
    }

    override suspend fun startStream() = create(stream = true)

    private suspend fun create(stream: Boolean) = withContext(dispatcher) {
        runCatching {
            stopStream()
            setDefaultStreamValues()
            kortholtHandle.set(nativeCreateKortholt(getExclusiveCores(), stream))
        }.isSuccess
    }

    override suspend fun stopStream() = withContext(dispatcher) {
        runCatching {
            kortholtHandle.getAndSet(NOT_SET).takeIf { it != NOT_SET }?.let { nativeDeleteKortholt(it) }
        }.isSuccess
    }

    override fun sendBang(receiver: String) {
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            nativeSendBang(handle, receiver)
        } else {
            android.util.Log.w("KortholtPlayer", "Cannot send bang to $receiver: stream not started")
        }
    }

    override fun sendFloat(receiver: String, x: Float) {
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            nativeSendFloat(handle, receiver, x)
        } else {
            android.util.Log.w("KortholtPlayer", "Cannot send float to $receiver: stream not started")
        }
    }

    override fun sendList(receiver: String, vararg args: Any) {
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            // For now, convert each arg to individual sends rather than implementing full list support
            args.forEach { arg ->
                when (arg) {
                    is Float -> nativeSendFloat(handle, receiver, arg)
                    is Double -> nativeSendFloat(handle, receiver, arg.toFloat())
                    is Int -> nativeSendFloat(handle, receiver, arg.toFloat())
                    is String -> nativeSendSymbol(handle, receiver, arg)
                    else -> nativeSendSymbol(handle, receiver, arg.toString())
                }
            }
        } else {
            android.util.Log.w("KortholtPlayer", "Cannot send list to $receiver: stream not started")
        }
    }

    override fun setFloatReceiver(receiver: String, callback: (Float) -> Unit) {
        android.util.Log.d("KortholtPlayer", "Setting float receiver for: $receiver")
        floatReceivers[receiver] = callback
        
        // TODO: Implement PdListener once classes are available
    }

    override fun setListReceiver(receiver: String, callback: (List<Any>) -> Unit) {
        android.util.Log.d("KortholtPlayer", "Setting list receiver for: $receiver")
        listReceivers[receiver] = callback
        
        // TODO: Implement PdListener once classes are available
    }

    override fun removeReceiver(receiver: String) {
        android.util.Log.d("KortholtPlayer", "Removing receiver for: $receiver")
        floatReceivers.remove(receiver)
        listReceivers.remove(receiver)
        
        // TODO: Remove PdListener once classes are available
    }

    @ExperimentalWaveFile
    override suspend fun saveWaveFile(
        outputFile: File,
        duration: Duration,
        startBang: String,
        stopBang: String
    ) = withContext(dispatcher) {
        runCatching {
            create(stream = false)
            nativeSaveWaveFile(
                kortholtHandle = kortholtHandle.get(),
                fileName = outputFile.absolutePath,
                duration = duration.inWholeMilliseconds,
                startBang = startBang,
                stopBang = stopBang
            )
        }.getOrDefault(0)
    }

    private fun setDefaultStreamValues() {
        runCatching {
            context.getSystemService<AudioManager>()?.let { am ->
                val sampleRate = am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toInt()
                val framesPerBurst = am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)?.toInt()
                if (sampleRate != null && framesPerBurst != null) {
                    nativeSetDefaultStreamValues(sampleRate, framesPerBurst)
                }
            }
        }
    }

    private fun getExclusiveCores() = runCatching { Process.getExclusiveCores() }.getOrDefault(intArrayOf())

    private external fun nativeCreateKortholt(cpuIds: IntArray, stream: Boolean): Long
    private external fun nativeDeleteKortholt(kortholtHandle: Long)
    private external fun nativeSetDefaultStreamValues(sampleRate: Int, framesPerBurst: Int)
    private external fun nativeSaveWaveFile(
        kortholtHandle: Long,
        fileName: String,
        duration: Long,
        startBang: String,
        stopBang: String
    ): Int
    private external fun nativeSendBang(kortholtHandle: Long, receiver: String)
    private external fun nativeSendFloat(kortholtHandle: Long, receiver: String, value: Float)
    private external fun nativeSendSymbol(kortholtHandle: Long, receiver: String, symbol: String)
    private external fun nativeOpenPatch(kortholtHandle: Long, patch: String, path: String): Boolean
    private external fun nativeAddToSearchPath(kortholtHandle: Long, path: String)

    companion object {
        private const val NOT_SET = -1L
        private const val VERSION = "3.4.0"
    }
}
