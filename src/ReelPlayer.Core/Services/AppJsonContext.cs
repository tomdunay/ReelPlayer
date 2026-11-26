using System.Text.Json.Serialization;

namespace ReelPlayer.Core.Services;

/// <summary>Source-generated JSON metadata avoids reflection warmup at startup.</summary>
[JsonSourceGenerationOptions(WriteIndented = true, UseStringEnumConverter = true)]
[JsonSerializable(typeof(AppSettings))]
[JsonSerializable(typeof(Dictionary<string, ResumeStore.Entry>))]
[JsonSerializable(typeof(string[]))]
internal sealed partial class AppJsonContext : JsonSerializerContext
{
}
