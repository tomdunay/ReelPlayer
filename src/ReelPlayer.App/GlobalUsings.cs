// WPF projects do not import System.IO implicitly (it would clash with System.Windows.Shapes.Path in XAML code).
global using System.IO;
global using Path = System.IO.Path;
