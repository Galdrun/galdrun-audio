using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Headless.XUnit;
using Avalonia.Threading;

namespace Multiroom.App.Tests;

public sealed class VisibilityWatchTests
{
    // A busy spinner runs only while it is shown, its page included: Avalonia kept the frame timer going for one on a
    // hidden page (VisibilityWatch).
    [AvaloniaFact]
    public void A_spinner_on_a_hidden_page_does_not_run()
    {
        var spinner = new Arc { Classes = { "busy" } };
        var page = new Grid { Children = { spinner } };
        var window = new Window { Content = page };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            Assert.True(VisibilityWatch.GetTrackShown(spinner)); // Set by the Arc.busy style (App.axaml).
            Assert.Contains("shown", spinner.Classes);
            page.IsVisible = false; Dispatcher.UIThread.RunJobs();
            Assert.DoesNotContain("shown", spinner.Classes);
            page.IsVisible = true; spinner.IsVisible = false; Dispatcher.UIThread.RunJobs();
            Assert.DoesNotContain("shown", spinner.Classes);
            spinner.IsVisible = true; Dispatcher.UIThread.RunJobs();
            Assert.Contains("shown", spinner.Classes);
        }
        finally { window.Close(); }
    }
}
