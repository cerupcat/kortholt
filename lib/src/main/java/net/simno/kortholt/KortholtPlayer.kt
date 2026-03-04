package net.simno.kortholt

import android.content.Context
import android.media.AudioManager
import android.os.Process
import androidx.annotation.RawRes
import androidx.core.content.getSystemService
import com.getkeepsafe.relinker.ReLinker
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.time.Duration
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.Job
import kotlinx.coroutines.withContext
import net.lingala.zip4j.ZipFile
import org.puredata.core.PdBase
import org.puredata.core.PdBaseLoader
import org.puredata.core.PdReceiver
// Using fully qualified names to avoid conflicts

@Singleton
internal class KortholtPlayer @Inject constructor(
    @ApplicationContext private val context: Context,
    private val dispatcher: CoroutineDispatcher,
    private var inputDeviceId: Int = Kortholt.Player.Builder.DEVICE_ID_UNSPECIFIED,
    private var outputDeviceId: Int = Kortholt.Player.Builder.DEVICE_ID_UNSPECIFIED
) : Kortholt.Player {

    private val patchHandle = AtomicLong(NOT_SET)
    private val kortholtHandle = AtomicLong(NOT_SET)

    // Message polling management following pd-for-android pattern
    private val pollingScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var messagePollingJob: Job? = null

    // Callbacks for receiving messages from Pure Data
    private val floatReceivers = mutableMapOf<String, (Float) -> Unit>()
    private val listReceivers = mutableMapOf<String, (List<Any>) -> Unit>()
    private val subscribedSymbols = mutableSetOf<String>()

    // PdReceiver implementation to handle messages from Pure Data
    private val pdReceiver = object : PdReceiver.Adapter() {
        override fun print(s: String) {
            android.util.Log.d("KortholtPlayer", "PD Print: $s")
        }

        override fun receiveFloat(source: String, x: Float) {
            // android.util.Log.d("KortholtPlayer", "Received float from $source: $x")
            floatReceivers[source]?.invoke(x)
        }

        override fun receiveList(source: String, vararg args: Any) {
            // android.util.Log.d("KortholtPlayer", "Received list from $source: ${args.toList()}")
            listReceivers[source]?.invoke(args.toList())
        }

        override fun receiveBang(source: String) {
            // android.util.Log.d("KortholtPlayer", "Received bang from $source")
            // Treat bang as a float with value 1.0
            floatReceivers[source]?.invoke(1.0f)
        }

        override fun receiveSymbol(source: String, symbol: String) {
            // android.util.Log.d("KortholtPlayer", "Received symbol from $source: $symbol")
            // Treat symbol as a list with the symbol as the only element
            listReceivers[source]?.invoke(listOf(symbol))
        }

        override fun receiveMessage(source: String, symbol: String, vararg args: Any) {
            // android.util.Log.d("KortholtPlayer", "Received message from $source ($symbol): ${args.toList()}")
            // Treat message as a list with symbol + args
            listReceivers[source]?.invoke(listOf(symbol) + args.toList())
        }
    }

    init {
        // Override PdBase loader to use our custom libraries BEFORE PdBase is accessed
        PdBaseLoader.loaderHandler = object : PdBaseLoader() {
            override fun load() {
                android.util.Log.d("KortholtPlayer", "Custom PdBaseLoader: loading pd and pdnative (Oboe implementation)")
                try {
                    ReLinker.loadLibrary(context, "pd", VERSION)
                    ReLinker.loadLibrary(context, "pdnative", VERSION)  // Our custom Oboe implementation
                } catch (e: Exception) {
                    android.util.Log.e("KortholtPlayer", "Failed to load pd libraries", e)
                    throw e
                }
            }
        }

        // Load the Kortholt native library
        ReLinker.loadLibrary(context, "kortholt", VERSION)

        android.util.Log.d("KortholtPlayer", "Native libraries loaded successfully")

        // Set up PdReceiver to handle messages from Pure Data
        // This MUST happen before C++ calls libpd_init_audio() or libpd will crash
        PdBase.setReceiver(pdReceiver)
        android.util.Log.d("KortholtPlayer", "PdReceiver initialized")
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
                    PdBase.addToSearchPath(dir.absolutePath)
                } else {
                    patchFile.outputStream().use { output -> input.copyTo(output) }
                }

                if (patchFile.exists()) {
                    try {
                        // Use PdBase to open the patch and get a handle
                        val pdHandle = PdBase.openPatch(patchFile)
                        patchHandle.set(pdHandle.toLong())
                        android.util.Log.d("KortholtPlayer", "Patch opened successfully: $patchName (handle=$pdHandle)")
                        true
                    } catch (e: Exception) {
                        android.util.Log.e("KortholtPlayer", "Failed to open patch: $patchName", e)
                        false
                    }
                } else {
                    android.util.Log.e("KortholtPlayer", "Patch file not found: ${patchFile.absolutePath}")
                    false
                }
            }
        }.getOrDefault(false)
    }

    override suspend fun closePatch() = withContext(dispatcher) {
        runCatching {
            patchHandle.getAndSet(NOT_SET).takeIf { it != NOT_SET }?.let { handle ->
                PdBase.closePatch(handle.toInt())
                android.util.Log.d("KortholtPlayer", "Closed patch (handle=${handle.toInt()})")
            }
        }.isSuccess
    }

    override suspend fun startStream() = create(stream = true)

    private suspend fun create(stream: Boolean) = withContext(dispatcher) {
        runCatching {
            stopStream()
            setDefaultStreamValues()
            android.util.Log.d("KortholtPlayer", "Creating stream with inputDeviceId=$inputDeviceId, outputDeviceId=$outputDeviceId")
            kortholtHandle.set(nativeCreateKortholt(getExclusiveCores(), stream, inputDeviceId, outputDeviceId))

            // Start message polling after successful stream creation (following pd-for-android pattern)
            if (kortholtHandle.get() != NOT_SET) {
                startMessagePolling()
            }
        }.isSuccess
    }

    override suspend fun stopStream() = withContext(dispatcher) {
        runCatching {
            // Stop message polling first (following pd-for-android pattern)
            stopMessagePolling()

            kortholtHandle.getAndSet(NOT_SET).takeIf { it != NOT_SET }?.let { nativeDeleteKortholt(it) }
        }.isSuccess
    }

    override fun sendBang(receiver: String) {
        if (kortholtHandle.get() == NOT_SET) {
            android.util.Log.w("KortholtPlayer", "Cannot send bang to $receiver: stream not started")
            return
        }
        val result = PdBase.sendBang(receiver)
        // android.util.Log.d("KortholtPlayer", "Sent bang to '$receiver': result=$result")
    }

    override fun sendFloat(receiver: String, x: Float) {
        if (kortholtHandle.get() == NOT_SET) {
            android.util.Log.w("KortholtPlayer", "Cannot send float to $receiver: stream not started")
            return
        }
        val result = PdBase.sendFloat(receiver, x)
        // android.util.Log.d("KortholtPlayer", "Sent float to '$receiver': $x (result=$result)")
    }

    override fun sendList(receiver: String, vararg args: Any) {
        if (kortholtHandle.get() == NOT_SET) {
            android.util.Log.w("KortholtPlayer", "Cannot send list to $receiver: stream not started")
            return
        }
        val result = PdBase.sendList(receiver, *args)
        // android.util.Log.d("KortholtPlayer", "Sent list to '$receiver': ${args.toList()} (result=$result)")
    }

    override fun setFloatReceiver(receiver: String, callback: (Float) -> Unit) {
        android.util.Log.d("KortholtPlayer", "Setting float receiver for: $receiver")
        floatReceivers[receiver] = callback

        // Subscribe to the symbol in Pure Data if not already subscribed
        if (subscribedSymbols.add(receiver)) {
            val result = PdBase.subscribe(receiver)
            // android.util.Log.d("KortholtPlayer", "Subscribed to '$receiver': result=$result")
        }
    }

    override fun setListReceiver(receiver: String, callback: (List<Any>) -> Unit) {
        android.util.Log.d("KortholtPlayer", "Setting list receiver for: $receiver")
        listReceivers[receiver] = callback

        // Subscribe to the symbol in Pure Data if not already subscribed
        if (subscribedSymbols.add(receiver)) {
            val result = PdBase.subscribe(receiver)
            android.util.Log.d("KortholtPlayer", "Subscribed to '$receiver': result=$result")
        }
    }

    override fun removeReceiver(receiver: String) {
        android.util.Log.d("KortholtPlayer", "Removing receiver for: $receiver")
        floatReceivers.remove(receiver)
        listReceivers.remove(receiver)

        // Unsubscribe from Pure Data if no receivers remain for this symbol
        if (subscribedSymbols.remove(receiver)) {
            PdBase.unsubscribe(receiver)
            android.util.Log.d("KortholtPlayer", "Unsubscribed from '$receiver'")
        }
    }

    override fun setRecorderCallback(recorderHandle: Long) {
        android.util.Log.d("KortholtPlayer", "Setting recorder callback: handle=$recorderHandle")
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            nativeSetRecorderCallback(handle, recorderHandle)
        } else {
            android.util.Log.w("KortholtPlayer", "Cannot set recorder callback: stream not started")
        }
    }

    override fun clearRecorderCallback() {
        android.util.Log.d("KortholtPlayer", "Clearing recorder callback")
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            nativeClearRecorderCallback(handle)
        } else {
            android.util.Log.w("KortholtPlayer", "Cannot clear recorder callback: stream not started")
        }
    }

    override fun getStreamSampleRate(): Int {
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            return nativeGetStreamSampleRate(handle)
        }
        android.util.Log.w("KortholtPlayer", "Cannot get sample rate: stream not started")
        return 0
    }

    override fun startStreams() {
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            android.util.Log.d("KortholtPlayer", "Starting streams")
            nativeStartStreams(handle)
        } else {
            android.util.Log.w("KortholtPlayer", "Cannot start streams: kortholt not created")
        }
    }

    override fun enableMicInput() {
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            android.util.Log.d("KortholtPlayer", "Enabling mic input")
            nativeEnableMicInput(handle)
        } else {
            android.util.Log.w("KortholtPlayer", "Cannot enable mic input: kortholt not created")
        }
    }

    override fun setDeviceIds(inputDeviceId: Int, outputDeviceId: Int) {
        android.util.Log.d("KortholtPlayer", "Setting device IDs: input=$inputDeviceId, output=$outputDeviceId")
        this.inputDeviceId = inputDeviceId
        this.outputDeviceId = outputDeviceId
        val handle = kortholtHandle.get()
        if (handle != NOT_SET) {
            nativeSetDeviceIds(handle, inputDeviceId, outputDeviceId)
        } else {
            android.util.Log.d("KortholtPlayer", "Stream not started, device IDs will be used on next stream creation")
        }
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

    private external fun nativeCreateKortholt(
        cpuIds: IntArray,
        stream: Boolean,
        inputDeviceId: Int,
        outputDeviceId: Int
    ): Long
    private external fun nativeDeleteKortholt(kortholtHandle: Long)
    private external fun nativeSetDefaultStreamValues(sampleRate: Int, framesPerBurst: Int)
    private external fun nativeSetRecorderCallback(kortholtHandle: Long, recorderHandle: Long)
    private external fun nativeClearRecorderCallback(kortholtHandle: Long)
    private external fun nativeSetDeviceIds(kortholtHandle: Long, inputDeviceId: Int, outputDeviceId: Int)
    private external fun nativeStartStreams(kortholtHandle: Long)
    private external fun nativeEnableMicInput(kortholtHandle: Long)
    private external fun nativeGetStreamSampleRate(kortholtHandle: Long): Int
    private external fun nativeSaveWaveFile(
        kortholtHandle: Long,
        fileName: String,
        duration: Long,
        startBang: String,
        stopBang: String
    ): Int

    /**
     * Start automatic message polling following pd-for-android pattern.
     * This polls the libpd message queue every 10ms and forwards messages to Java receivers.
     */
    private fun startMessagePolling() {
        stopMessagePolling() // Stop any existing polling

        messagePollingJob = pollingScope.launch {
            android.util.Log.d("KortholtPlayer", "Message polling started (pd-for-android pattern)")

            while (true) {
                try {
                    // Poll libpd message queue - this is equivalent to what PdAudio does automatically
                    PdBase.pollPdMessageQueue()
                } catch (e: Exception) {
                    android.util.Log.e("KortholtPlayer", "Error polling PD messages: ${e.message}")
                }

                // 10ms polling interval for low latency (similar to pd-for-android's 20ms timer)
                delay(10)
            }
        }
    }

    /**
     * Stop automatic message polling.
     */
    private fun stopMessagePolling() {
        messagePollingJob?.cancel()
        messagePollingJob = null
        android.util.Log.d("KortholtPlayer", "Message polling stopped")
    }

    companion object {
        private const val NOT_SET = -1L
        private const val VERSION = "3.4.0"
    }
}
