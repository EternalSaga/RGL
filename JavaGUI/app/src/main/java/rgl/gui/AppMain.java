package rgl.gui;

import javafx.application.Application;
import javafx.scene.Scene;
import javafx.scene.canvas.Canvas;
import javafx.scene.control.Label;
import javafx.scene.image.PixelBuffer;
import javafx.scene.layout.StackPane;
import javafx.stage.Stage;

public class AppMain extends Application{

    private static final int WIDTH = 720;
    private static final int HEIGHT = 480;
    private static final int CHANNEL = 3;
   
    @Override
    public void start(Stage stage) {
        String javaVersion = System.getProperty("java.version");
        String javafxVersion = System.getProperty("javafx.version");
        private PixelBuffer pixelBuffer = new PixelBuffer<Buffer>(WIDTH, HEIGHT, null, null);
    }

    public static void main(String[] args) {
        launch();
    }
}