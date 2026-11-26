using System.Globalization;
using System.Windows.Data;

namespace ReelPlayer.App.Controls;

/// <summary>(Value, Minimum, Maximum, ActualWidth) → pixel width of the filled part of a slider track.</summary>
public sealed class SliderFillConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type targetType, object? parameter, CultureInfo culture)
    {
        if (values.Length < 4 || values[0] is not double value || values[1] is not double min ||
            values[2] is not double max || values[3] is not double width || max <= min || width <= 0)
            return 0.0;
        return Math.Clamp((value - min) / (max - min), 0, 1) * width;
    }

    public object[] ConvertBack(object value, Type[] targetTypes, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
