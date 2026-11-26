using System.Windows;
using System.Windows.Controls;
using ReelPlayer.App.ViewModels;

namespace ReelPlayer.App.Views;

public partial class EffectsWindow : Window
{
    private readonly MainViewModel _vm;
    private readonly List<Slider> _bands = new();
    private Slider? _preamp;
    private bool _loading = true;

    public EffectsWindow(MainViewModel vm)
    {
        _vm = vm;
        InitializeComponent();
        BuildEqualizer();

        EqEnable.IsChecked = vm.EqEnabled;
        PresetBox.Items.Add("(custom)");
        foreach (var p in MainViewModel.EqualizerPresets()) PresetBox.Items.Add(p);
        PresetBox.SelectedIndex = vm.EqPreset + 1;

        AdjustEnable.IsChecked = vm.AdjustEnabled;
        Brightness.Value = vm.Brightness;
        Contrast.Value = vm.Contrast;
        Hue.Value = vm.Hue;
        Saturation.Value = vm.Saturation;
        Gamma.Value = vm.Gamma;

        foreach (var t in MainViewModel.Transforms) TransformBox.Items.Add(t.Name);
        TransformBox.SelectedIndex = vm.TransformIndex;
        SharpenEnable.IsChecked = vm.Sharpen;
        SharpenSigma.Value = vm.SharpenSigma;
        _loading = false;
    }

    private void BuildEqualizer()
    {
        var labels = MainViewModel.EqualizerBandLabels();
        BandGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        BandGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        BandGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        _preamp = AddBand(0, "Preamp", _vm.EqPreamp, v => _vm.SetEqualizerPreamp(v));
        BandGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
        for (var i = 0; i < labels.Count && i < _vm.EqBands.Length; i++)
        {
            var band = i;
            _bands.Add(AddBand(i + 2, labels[i] + "Hz", _vm.EqBands[i], v =>
            {
                _vm.SetEqualizerBand(band, v);
                SetPresetSilently(0);
            }));
        }
    }

    private Slider AddBand(int column, string label, float value, Action<float> onChange)
    {
        while (BandGrid.ColumnDefinitions.Count <= column)
            BandGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var slider = new Slider
        {
            Orientation = Orientation.Vertical, Minimum = -20, Maximum = 20, Value = value,
            HorizontalAlignment = HorizontalAlignment.Center, TickFrequency = 10, TickPlacement = System.Windows.Controls.Primitives.TickPlacement.BottomRight,
            SmallChange = 0.5, LargeChange = 2,
        };
        var valueText = new TextBlock { HorizontalAlignment = HorizontalAlignment.Center, FontSize = 11, Text = $"{value:+0.0;-0.0;0} dB" };
        slider.ValueChanged += (_, e) =>
        {
            valueText.Text = $"{e.NewValue:+0.0;-0.0;0} dB";
            if (!_loading) onChange((float)e.NewValue);
        };
        var name = new TextBlock { Text = label, HorizontalAlignment = HorizontalAlignment.Center, FontSize = 12, Margin = new Thickness(0, 4, 0, 0) };

        Grid.SetColumn(slider, column);
        Grid.SetColumn(valueText, column); Grid.SetRow(valueText, 1);
        Grid.SetColumn(name, column); Grid.SetRow(name, 2);
        BandGrid.Children.Add(slider);
        BandGrid.Children.Add(valueText);
        BandGrid.Children.Add(name);
        return slider;
    }

    private void SetPresetSilently(int index)
    {
        var was = _loading;
        _loading = true;
        PresetBox.SelectedIndex = index;
        _loading = was;
    }

    private void EqEnable_Changed(object sender, RoutedEventArgs e)
    {
        if (!_loading) _vm.SetEqualizerEnabled(EqEnable.IsChecked == true);
    }

    private void PresetBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || PresetBox.SelectedIndex <= 0) return;
        var values = _vm.LoadEqualizerPreset(PresetBox.SelectedIndex - 1);
        _loading = true;
        for (var i = 0; i < _bands.Count && i < values.Length; i++) _bands[i].Value = values[i];
        if (_preamp is not null) _preamp.Value = _vm.EqPreamp;
        _loading = false;
        if (EqEnable.IsChecked != true) EqEnable.IsChecked = true;
    }

    private void Adjust_Changed(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _vm.SetAdjust(AdjustEnable.IsChecked == true, (float)Brightness.Value, (float)Contrast.Value,
            (float)Hue.Value, (float)Saturation.Value, (float)Gamma.Value);
    }

    private void ResetAdjust_Click(object sender, RoutedEventArgs e)
    {
        _loading = true;
        Brightness.Value = 1; Contrast.Value = 1; Hue.Value = 0; Saturation.Value = 1; Gamma.Value = 1;
        _loading = false;
        Adjust_Changed(sender, e);
    }

    private void ApplyFilters_Click(object sender, RoutedEventArgs e) =>
        _vm.SetVideoFilters(Math.Max(0, TransformBox.SelectedIndex), SharpenEnable.IsChecked == true, SharpenSigma.Value);

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
