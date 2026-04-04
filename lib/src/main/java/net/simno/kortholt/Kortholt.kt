package net.simno.kortholt

import android.content.Context
import androidx.annotation.RawRes
import java.io.File
import kotlin.time.Duration
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers

object Kortholt {

    private var factory: Player.Factory? = null
    private var player: Player? = null

    @JvmStatic
    fun player(context: Context): Player = player ?: newPlayer(context)

    @JvmStatic
    @Synchronized
    fun setPlayer(player: Player) {
        this.factory = null
        this.player = player
    }

    @JvmStatic
    @Synchronized
    fun setPlayer(factory: Player.Factory) {
        this.factory = factory
        this.player = null
    }

    @JvmStatic
    @Synchronized
    fun reset() {
        this.factory = null
        this.player = null
    }

    @Synchronized
    private fun newPlayer(context: Context): Player {
        player?.let { return it }

        val newPlayer = factory?.newPlayer()
            ?: (context.applicationContext as? Player.Factory)?.newPlayer()
            ?: Player.Builder(context).build()
        factory = null
        player = newPlayer

        return newPlayer
    }

    interface Player {
        suspend fun openPatch(
            @RawRes patchRes: Int,
            patchName: String,
            extractZip: Boolean = false
        ): Boolean

        suspend fun closePatch(): Boolean
        suspend fun startStream(): Boolean
        suspend fun stopStream(): Boolean
        fun sendBang(receiver: String)
        fun sendFloat(receiver: String, x: Float)
        fun sendList(receiver: String, vararg args: Any)
        
        // Message receiving from Pure Data
        fun setFloatReceiver(receiver: String, callback: (Float) -> Unit)
        fun setListReceiver(receiver: String, callback: (List<Any>) -> Unit)
        fun removeReceiver(receiver: String)

        /**
         * Start output audio streams. Call after patch is opened to avoid
         * a race between audio processing and patch loading.
         */
        fun startStreams()

        /**
         * Create and start the input (microphone) stream.
         * Call when RECORD_AUDIO permission is granted.
         * Safe to call while output stream is already running.
         */
        fun enableMicInput()

        // Audio recorder integration
        fun setRecorderCallback(recorderHandle: Long)
        fun clearRecorderCallback()

        // Reverb effect control for recording
        fun setReverbEnabled(enabled: Boolean)
        fun setReverbLevel(level: Float)

        /**
         * Get the sample rate of the active output stream.
         * This is the rate at which PureData and audio recording operate.
         * The rate depends on the device (typically 44100 or 48000 Hz).
         * Returns 0 if the stream is not initialized.
         */
        fun getStreamSampleRate(): Int

        /**
         * Set audio device IDs for input and output.
         * This will restart the audio streams with the new devices.
         * Use [Builder.DEVICE_ID_UNSPECIFIED] for system default.
         * @param inputDeviceId Oboe device ID for microphone input
         * @param outputDeviceId Oboe device ID for speaker/headphone output
         */
        fun setDeviceIds(inputDeviceId: Int, outputDeviceId: Int)

        /**
         * Check if the input stream is producing digital silence.
         * Returns true when consecutive all-zero audio callbacks exceed the
         * configured threshold, indicating a device-specific AAudio bug.
         */
        fun isInputDigitalSilence(): Boolean

        /**
         * Close and reopen the input stream with a different InputPreset and/or AudioApi.
         * The output stream, PD patch, and all state remain untouched.
         *
         * @param preset Oboe InputPreset ordinal value
         * @param audioApi Oboe AudioApi ordinal value (0 = Unspecified, let Oboe choose)
         */
        fun reopenInputStream(preset: Int, audioApi: Int)

        /**
         * Reset silence detection counters without reopening the stream.
         */
        fun resetInputSilenceDetection()

        @ExperimentalWaveFile
        suspend fun saveWaveFile(
            outputFile: File,
            duration: Duration,
            startBang: String = "",
            stopBang: String = ""
        ): Int

        fun interface Factory {
            fun newPlayer(): Player
        }

        class Builder(
            context: Context
        ) {
            private val applicationContext: Context = context.applicationContext
            private var dispatcher: CoroutineDispatcher = Dispatchers.IO
            private var inputDeviceId: Int = DEVICE_ID_UNSPECIFIED
            private var outputDeviceId: Int = DEVICE_ID_UNSPECIFIED

            fun dispatcher(dispatcher: CoroutineDispatcher): Builder = apply {
                this.dispatcher = dispatcher
            }

            /**
             * Set the input audio device ID (microphone).
             * Use [DEVICE_ID_UNSPECIFIED] for system default.
             * Device IDs can be obtained from [android.media.AudioDeviceInfo.getId].
             */
            fun inputDeviceId(deviceId: Int): Builder = apply {
                this.inputDeviceId = deviceId
            }

            /**
             * Set the output audio device ID (speaker/headphones).
             * Use [DEVICE_ID_UNSPECIFIED] for system default.
             * Device IDs can be obtained from [android.media.AudioDeviceInfo.getId].
             */
            fun outputDeviceId(deviceId: Int): Builder = apply {
                this.outputDeviceId = deviceId
            }

            fun build(): Player = KortholtPlayer(
                context = applicationContext,
                dispatcher = dispatcher,
                inputDeviceId = inputDeviceId,
                outputDeviceId = outputDeviceId
            )

            companion object {
                /**
                 * Use system default audio device.
                 * Matches oboe::kUnspecified in the native layer.
                 */
                const val DEVICE_ID_UNSPECIFIED = 0
            }
        }
    }
}
